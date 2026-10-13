#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "khook.h"
#include "util.h"

#define FW_1270             0x12700000U
#define SYSTEM_AUTHID       0x4800000000000007ULL
#define HOOK_CORE           0
#define HOOK_IST            6
#define CAVE_STATE_OFF      0x600ULL
#define PCPU_ARRAY_OFFSET   0x2e9cf00ULL
#define PCPU_STRIDE         0x980ULL
#define SECOND_MARKER       0x1270DBDB5EEDBEEFULL
#define BIG_SAVE_BYTES      0x600U
#define MAX_AWAKE_POLLS     600U
#define MAX_RESUME_POLLS    15U

static uint8_t saved_head[KH_SCRATCH_MIN];
static uint8_t saved_big[BIG_SAVE_BYTES];
static char output_path[128];

static int read_qword(uint64_t addr, uint64_t *value)
{
  *value = 0;
  return kread(addr, value, sizeof(*value));
}

static int save_line(const char *line, int create)
{
  int flags = O_WRONLY | (create ? O_CREAT | O_EXCL : O_APPEND);
  int fd = open(output_path, flags, 0600);
  if (fd < 0)
    return -1;
  size_t length = strlen(line);
  ssize_t written = write(fd, line, length);
  int synced = written == (ssize_t)length ? fsync(fd) : -1;
  int closed = close(fd);
  return written == (ssize_t)length && synced == 0 && closed == 0 ? 0 : -1;
}

static int save_line_retry(const char *line)
{
  for (unsigned attempt = 0; attempt < 60; ++attempt) {
    if (save_line(line, 0) == 0)
      return 0;
    sleep(1);
  }
  return -1;
}

static int cleanup(khook_t *hook, int armed, uint64_t cave,
                   uint64_t authid, int restore_cave)
{
  int disarm = 0, gate = 0, head = 0, big = 0, marker = 0;
  if (armed) {
    intptr_t thread = kernel_get_proc_thread(getpid(), -1);
    uint64_t row_thread = 0;
    uint64_t row = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                   PCPU_ARRAY_OFFSET + HOOK_CORE * PCPU_STRIDE;
    if (pin_to_core(HOOK_CORE) != 0 || !thread ||
        read_qword(row, &row_thread) != 0 || row_thread != (uint64_t)thread)
      disarm = -1;
    else
      disarm = kh_disarm(hook);
  }
  if (hook->installed && disarm == 0)
    gate = kh_restore(hook);
  if (restore_cave && disarm == 0 && gate == 0) {
    head = kwrite(cave, saved_head, sizeof(saved_head));
    big = kwrite(cave + KH_OFF_BIG_CHAIN, saved_big, sizeof(saved_big));
    marker = kwrite(cave + KH_OFF_USER, saved_head + KH_OFF_USER, 8);
  }
  int auth = kernel_set_ucred_authid(getpid(), authid);
  klog("RSH1270,1,cleanup,disarm=%d,gate=%d,head=%d,big=%d,marker=%d,auth=%d\n",
       disarm, gate, head, big, marker, auth);
  return disarm == 0 && gate == 0 && head == 0 && big == 0 &&
         marker == 0 && auth == 0 ? 0 : -1;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog("RSH1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 2;
  }

  uint64_t cave = kdlsym(KERNEL_SYM_DATA_CAVE);
  uint64_t target = kdlsym(KERNEL_SYM_HV_MP_STARTUP_CALL);
  uint64_t stock_vector = kdlsym(KERNEL_SYM_MP_STARTUP_INIT_VECTOR);
  uint64_t pmap = kdlsym(KERNEL_SYM_PMAP_STORE);
  uint64_t cr3 = 0, state = 0, marker_before = 0;
  if (target < ktext(0) || target >= ktext(KH_KIMG_MAX) ||
      stock_vector < ktext(0) || stock_vector >= ktext(KH_KIMG_MAX) ||
      read_qword(pmap + 0x28, &cr3) != 0 || cr3 == 0 || (cr3 & 0xfff) ||
      read_qword(cave + CAVE_STATE_OFF, &state) != 0 || state != 0 ||
      read_qword(cave + KH_OFF_USER, &marker_before) != 0 ||
      marker_before != 0) {
    klog("RSH1270,1,stop,stage=preflight,target=0x%016" PRIx64
         ",vector=0x%016" PRIx64 ",cr3=0x%016" PRIx64
         ",state=0x%016" PRIx64 ",marker=0x%016" PRIx64 "\n",
         target, stock_vector, cr3, state, marker_before);
    return 3;
  }

  uint64_t authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), SYSTEM_AUTHID) != 0 ||
      pin_to_core(HOOK_CORE) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    klog("RSH1270,1,stop,stage=authid-or-affinity\n");
    return 4;
  }

  intptr_t thread = kernel_get_proc_thread(getpid(), -1);
  uint64_t row_thread = 0;
  uint64_t row = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                 PCPU_ARRAY_OFFSET + HOOK_CORE * PCPU_STRIDE;
  if (!thread || read_qword(row, &row_thread) != 0 ||
      row_thread != (uint64_t)thread ||
      kread(cave, saved_head, sizeof(saved_head)) != 0 ||
      kread(cave + KH_OFF_BIG_CHAIN, saved_big, sizeof(saved_big)) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    klog("RSH1270,1,stop,stage=thread-or-backup,thread=0x%016" PRIx64
         ",row_thread=0x%016" PRIx64 "\n",
         (uint64_t)thread, row_thread);
    return 5;
  }

  struct timespec stamp = {0};
  if (clock_gettime(CLOCK_REALTIME, &stamp) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    return 6;
  }
  snprintf(output_path, sizeof(output_path),
           "/mnt/usb0/PS5-12.70-stock-resume-%d-%ld-%ld.txt",
           (int)getpid(), (long)stamp.tv_sec, (long)stamp.tv_nsec);
  char line[256];
  snprintf(line, sizeof(line),
           "prepared,pid=%d,target=0x%016" PRIx64
           ",stock_vector=0x%016" PRIx64 ",cr3=0x%016" PRIx64 "\n",
           (int)getpid(), target, stock_vector, cr3);
  if (save_line(line, 1) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    klog("RSH1270,1,stop,stage=usb-create,path=%s\n", output_path);
    return 7;
  }

  khook_t hook;
  int result = kh_init(&hook, kread, kwrite, ktext(0), cave, HOOK_IST);
  if (result != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    return 8;
  }
  hook.tss_stride = KH_TSS_STRIDE;
  kh_set_tables(&hook, kdlsym(KERNEL_SYM_IDT_ARRAY),
                kdlsym(KERNEL_SYM_COMMON_TSS));
  kh_set_core(&hook, HOOK_CORE);
  kh_set_hook_target(&hook, target);
  hook.oneshot = 1;
  kh_set_resume_rsi(&hook, stock_vector);
  kh_set_resume_rdx(&hook, cr3);

  klog("RSH1270,1,start,target=0x%016" PRIx64
       ",vector=0x%016" PRIx64 ",cr3=0x%016" PRIx64 "\n",
       target, stock_vector, cr3);
  if (save_line("hook,before-install\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 0);
    return 9;
  }
  result = kh_install(&hook, NULL);
  if (result != 0) {
    klog("RSH1270,1,stop,stage=install,result=%d\n", result);
    save_line("hook,install-failed\n", 0);
    cleanup(&hook, 0, cave, authid, 1);
    return 10;
  }
  if (save_line("hook,after-install\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 1);
    return 11;
  }

  uint64_t chain[5] = {0};
  int chain_ok = kread(cave + KH_OFF_BIG_CHAIN, chain, sizeof(chain)) == 0 &&
                 chain[0] == kgadget(KERNEL_GADGET_POP_RCX) &&
                 chain[1] == cave + KH_OFF_USER &&
                 chain[2] == kgadget(KERNEL_GADGET_POP_RAX) &&
                 chain[3] == SECOND_MARKER &&
                 chain[4] == kgadget(KERNEL_GADGET_ST_RCX_RAX);
  if (!chain_ok) {
    save_line("hook,marker-chain-mismatch\n", 0);
    cleanup(&hook, 0, cave, authid, 1);
    return 12;
  }

#ifdef RESUME_STOCK_PREP_ONLY_1270
  int prep_cleanup = cleanup(&hook, 0, cave, authid, 1);
  save_line(prep_cleanup == 0 ? "end,prepare-only=ok\n" :
            "end,prepare-only=cleanup-failed\n", 0);
  klog("RSH1270,1,end,prepare-only,cleanup=%d\n", prep_cleanup);
  return prep_cleanup == 0 ? 0 : 21;
#endif

  if (save_line("hook,before-trigger\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 1);
    return 13;
  }
  result = kh_trigger(&hook);
  if (result != 0) {
    save_line("hook,trigger-failed\n", 0);
    cleanup(&hook, 1, cave, authid, 1);
    return 14;
  }
  if (save_line("hook,after-trigger\n", 0) != 0) {
    cleanup(&hook, 1, cave, authid, 1);
    return 15;
  }

  uint64_t marker = 0;
  if (read_qword(cave + KH_OFF_USER, &marker) != 0 || marker != 0) {
    save_line("hook,unexpected-early-marker\n", 0);
    cleanup(&hook, 1, cave, authid, 1);
    return 16;
  }

  if (save_line("ready,enter-rest-mode\n", 0) != 0) {
    cleanup(&hook, 1, cave, authid, 1);
    return 17;
  }
  klog("RSH1270,1,ready,path=%s\n", output_path);
  notifyf("12.70 stock-vector probe ready\nEnter rest mode, then resume");

  struct timespec previous = {0};
  clock_gettime(CLOCK_REALTIME, &previous);
  unsigned resumed_polls = 0;
  int saw_resume_gap = 0;
  for (unsigned awake = 0; awake < MAX_AWAKE_POLLS; ++awake) {
    sleep(1);
    struct timespec now = {0};
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
      int64_t gap = (int64_t)now.tv_sec - (int64_t)previous.tv_sec;
      if (gap >= 10 && !saw_resume_gap) {
        saw_resume_gap = 1;
        klog("RSH1270,1,resume-gap,seconds=%ld\n", (long)gap);
      }
      previous = now;
    }
    if (read_qword(cave + KH_OFF_USER, &marker) != 0) {
      save_line_retry("stop,marker-read-failed\n");
      cleanup(&hook, 1, cave, authid, 1);
      return 18;
    }
    if (marker == SECOND_MARKER || (saw_resume_gap && ++resumed_polls >= MAX_RESUME_POLLS))
      break;
    if (marker != 0) {
      save_line_retry("stop,unexpected-marker\n");
      cleanup(&hook, 1, cave, authid, 1);
      return 19;
    }
  }

  int fired = marker == SECOND_MARKER;
  int cleanup_result = cleanup(&hook, !fired, cave, authid, 1);
  snprintf(line, sizeof(line),
           "result,marker=0x%016" PRIx64 ",fired=%d,resume_gap=%d,cleanup=%d\n",
           marker, fired, saw_resume_gap, cleanup_result);
  int saved = save_line_retry(line);
  klog("RSH1270,1,%s", line);
  notifyf("12.70 stock-vector probe\n%s", fired ? "breakpoint fired" : "no breakpoint marker");
  // CLOCK_REALTIME may not expose the S3 pause to this polling thread.
  // The marker, cleanup, and durable result are the probe's success criteria.
  return fired && cleanup_result == 0 && saved == 0 ? 0 : 20;
}
