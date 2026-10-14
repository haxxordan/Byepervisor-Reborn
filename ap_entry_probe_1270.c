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
#include "carrier_1270.h"
#include "util.h"

#if !defined(AP_ENTRY_PREP_ONLY_1270) && !defined(AP_ENTRY_REENABLE_AFTER_TWO_KPS_1270)
#error "12.70 owned-page AP entry KP'd with and without a stack; do not rebuild unchanged"
#endif

#define FW_1270             0x12700000U
#define SYSTEM_AUTHID       0x4800000000000007ULL
#define HOOK_CORE           0
#define HOOK_IST            6
#define CAVE_STATE_OFF      0x600ULL
#define PCPU_ARRAY_OFFSET   0x2e9cf00ULL
#define PCPU_STRIDE         0x980ULL
#define SECOND_MARKER       0x1270DBDB5EEDBEEFULL
#define AP_MARKER           0x1270A9E17E5E0001ULL
#define AP_CR3_PA           0x68000ULL
#define AP_STUB_OFFSET      0x100ULL
#define AP_STUB_BYTES       42U
#define AP_PATCH_BYTES      64U
#define FRAME_4K            0x000FFFFFFFFFF000ULL
#define FRAME_2M            0x000FFFFFFFE00000ULL
#define NX_BIT              (1ULL << 63)
#define TABLE_PA_LIMIT      0x400000000ULL
#define BIG_SAVE_BYTES      0x600U
#define MAX_AWAKE_POLLS     600U
#define MAX_RESUME_POLLS    60U

static uint8_t saved_head[KH_SCRATCH_MIN];
static uint8_t saved_big[BIG_SAVE_BYTES];
static uint8_t saved_carrier[AP_PATCH_BYTES];
static uint8_t ap_stub[AP_STUB_BYTES];
static char output_path[128];
static carrier1270_t owned_carrier;

static int read_qword(uint64_t addr, uint64_t *value);

static int ap_map_check(uint64_t va, int require_identity, int require_write)
{
  uint64_t pml4e = 0, pdpte = 0, pde = 0;
  uint64_t dmap = kdldmap(0);
  if (read_qword(dmap + AP_CR3_PA + (((va >> 39) & 0x1ff) * 8), &pml4e) != 0 ||
      !(pml4e & 1) || (pml4e & 0x80) ||
      (pml4e & FRAME_4K) >= TABLE_PA_LIMIT ||
      read_qword(dmap + (pml4e & FRAME_4K) +
                 (((va >> 30) & 0x1ff) * 8), &pdpte) != 0 ||
      !(pdpte & 1) || (pdpte & 0x80) ||
      (pdpte & FRAME_4K) >= TABLE_PA_LIMIT ||
      read_qword(dmap + (pdpte & FRAME_4K) +
                 (((va >> 21) & 0x1ff) * 8), &pde) != 0 ||
      (pde & 0x81) != 0x81 || ((pml4e | pdpte | pde) & NX_BIT) ||
      (require_write && (!(pml4e & 2) || !(pdpte & 2) || !(pde & 2))))
    return -1;
  uint64_t pa = (pde & FRAME_2M) | (va & 0x1fffff);
  return pa < 0x40000000ULL && (!require_identity || pa == va) ? 0 : -1;
}

static int rollback_ap_stub(uint64_t carrier)
{
  uint8_t observed[AP_PATCH_BYTES] = {0};
  if (kwrite(carrier, saved_carrier, sizeof(saved_carrier)) != 0 ||
      kread(carrier, observed, sizeof(observed)) != 0 ||
      memcmp(observed, saved_carrier, sizeof(observed)) != 0)
    return -1;
  return 0;
}

static int stage_ap_stub(uint64_t carrier, uint64_t oracle, uint64_t vector)
{
  uint64_t magic = AP_MARKER;
  static const uint8_t template[AP_STUB_BYTES] = {
    0x48, 0x8d, 0x25, 0xf9, 0x0d, 0x00, 0x00, // lea rsp, [rip+0xdf9] -> page+0xf00
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax, marker address
    0x48, 0xba, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rdx, marker value
    0x48, 0x89, 0x10,                   // mov [rax], rdx
    0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rcx, stock vector
    0xff, 0xe1                          // jmp rcx
  };
  memcpy(ap_stub, template, sizeof(ap_stub));
  memcpy(ap_stub + 9, &oracle, 8);
  memcpy(ap_stub + 19, &magic, 8);
  memcpy(ap_stub + 32, &vector, 8);
  if (kread(carrier, saved_carrier, sizeof(saved_carrier)) != 0)
    return -1;
  for (unsigned i = 0; i < sizeof(saved_carrier); ++i)
    if (saved_carrier[i] != 0)
      return -2;
  sleep(1);
  uint8_t stable[AP_PATCH_BYTES] = {0};
  if (kread(carrier, stable, sizeof(stable)) != 0 ||
      memcmp(stable, saved_carrier, sizeof(stable)) != 0)
    return -2;
  if (kwrite(carrier, ap_stub, sizeof(ap_stub)) != 0) {
    return rollback_ap_stub(carrier) == 0 ? -3 : -5;
  }
  uint8_t observed[AP_STUB_BYTES] = {0};
  if (kread(carrier, observed, sizeof(observed)) != 0 ||
      memcmp(observed, ap_stub, sizeof(observed)) != 0) {
    return rollback_ap_stub(carrier) == 0 ? -4 : -5;
  }
  return 0;
}

static int restore_ap_stub(uint64_t carrier)
{
  uint8_t observed[AP_STUB_BYTES] = {0};
  if (kread(carrier, observed, sizeof(observed)) != 0 ||
      memcmp(observed, ap_stub, sizeof(observed)) != 0)
    return -1;
  return rollback_ap_stub(carrier);
}

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
                   uint64_t authid, int restore_cave, int restore_carrier)
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
  int carrier = restore_carrier ?
      restore_ap_stub(kdldmap(owned_carrier.pa + AP_STUB_OFFSET)) : 0;
  int release = carrier1270_release(&owned_carrier);
  int auth = kernel_set_ucred_authid(getpid(), authid);
  klog("APE1270,1,cleanup,disarm=%d,gate=%d,head=%d,big=%d,marker=%d,carrier=%d,release=%d,auth=%d\n",
       disarm, gate, head, big, marker, carrier, release, auth);
  return disarm == 0 && gate == 0 && head == 0 && big == 0 &&
         marker == 0 && carrier == 0 && release == 0 && auth == 0 ? 0 : -1;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog("APE1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 2;
  }

  uint64_t cave = kdlsym(KERNEL_SYM_DATA_CAVE);
  uint64_t target = kdlsym(KERNEL_SYM_HV_MP_STARTUP_CALL);
  uint64_t stock_vector = kdlsym(KERNEL_SYM_MP_STARTUP_INIT_VECTOR);
  uint64_t pmap = kdlsym(KERNEL_SYM_PMAP_STORE);
  uint64_t cr3 = 0, state = 0, marker_before = 0, ap_before = 0;
  if (target < ktext(0) || target >= ktext(KH_KIMG_MAX) ||
      stock_vector < ktext(0) || stock_vector >= ktext(KH_KIMG_MAX) ||
      read_qword(pmap + 0x28, &cr3) != 0 || cr3 == 0 || (cr3 & 0xfff) ||
      read_qword(cave + CAVE_STATE_OFF, &state) != 0 || state != 0 ||
      read_qword(cave + KH_OFF_USER, &marker_before) != 0 ||
      marker_before != 0 ||
      read_qword(cave + KH_OFF_USER + 8, &ap_before) != 0 ||
      ap_before != 0 ||
      ap_map_check(stock_vector, 0, 0) != 0 ||
      ap_map_check(cave + KH_OFF_USER + 8, 0, 1) != 0) {
    klog("APE1270,1,stop,stage=preflight,target=0x%016" PRIx64
         ",vector=0x%016" PRIx64 ",cr3=0x%016" PRIx64
         ",state=0x%016" PRIx64 ",marker=0x%016" PRIx64
         ",ap_marker=0x%016" PRIx64 "\n",
         target, stock_vector, cr3, state, marker_before, ap_before);
    return 3;
  }

  uint64_t authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), SYSTEM_AUTHID) != 0 ||
      pin_to_core(HOOK_CORE) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    klog("APE1270,1,stop,stage=authid-or-affinity\n");
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
    klog("APE1270,1,stop,stage=thread-or-backup,thread=0x%016" PRIx64
         ",row_thread=0x%016" PRIx64 "\n",
         (uint64_t)thread, row_thread);
    return 5;
  }

  int acquire = carrier1270_acquire(&owned_carrier, kdldmap(0));
  uint64_t stub_va = owned_carrier.pa + AP_STUB_OFFSET;
  uint64_t carrier_pa_after = 0;
  if (acquire != 0 || stub_va >= 0x40000000ULL ||
      ap_map_check(stub_va, 1, 0) != 0 ||
      carrier1270_translate(&owned_carrier, &carrier_pa_after) != 0 ||
      carrier_pa_after != owned_carrier.pa) {
    klog("APE1270,1,stop,stage=owned-carrier,acquire=%d,pa=0x%016" PRIx64
         ",pa_after=0x%016" PRIx64 "\n",
         acquire, owned_carrier.pa, carrier_pa_after);
    carrier1270_release(&owned_carrier);
    kernel_set_ucred_authid(getpid(), authid);
    return 5;
  }
  klog("APE1270,1,carrier,owned=1,pa=0x%016" PRIx64
       ",stub=0x%016" PRIx64 "\n", owned_carrier.pa, stub_va);

  struct timespec stamp = {0};
  if (clock_gettime(CLOCK_REALTIME, &stamp) != 0) {
    carrier1270_release(&owned_carrier);
    kernel_set_ucred_authid(getpid(), authid);
    return 6;
  }
  snprintf(output_path, sizeof(output_path),
           "/mnt/usb0/PS5-12.70-ap-entry-owned-%d-%ld-%ld.txt",
           (int)getpid(), (long)stamp.tv_sec, (long)stamp.tv_nsec);
  char line[256];
  snprintf(line, sizeof(line),
           "prepared,pid=%d,target=0x%016" PRIx64
           ",stock_vector=0x%016" PRIx64 ",ap_cr3=0x%016" PRIx64
           ",stub=0x%016" PRIx64 "\n",
           (int)getpid(), target, stock_vector,
           (uint64_t)AP_CR3_PA, stub_va);
  if (save_line(line, 1) != 0) {
    carrier1270_release(&owned_carrier);
    kernel_set_ucred_authid(getpid(), authid);
    klog("APE1270,1,stop,stage=usb-create,path=%s\n", output_path);
    return 7;
  }

  khook_t hook;
  int result = kh_init(&hook, kread, kwrite, ktext(0), cave, HOOK_IST);
  if (result != 0) {
    carrier1270_release(&owned_carrier);
    kernel_set_ucred_authid(getpid(), authid);
    return 8;
  }
  hook.tss_stride = KH_TSS_STRIDE;
  kh_set_tables(&hook, kdlsym(KERNEL_SYM_IDT_ARRAY),
                kdlsym(KERNEL_SYM_COMMON_TSS));
  kh_set_core(&hook, HOOK_CORE);
  kh_set_hook_target(&hook, target);
  hook.oneshot = 1;
  kh_set_resume_rsi(&hook, stub_va);
  kh_set_resume_rdx(&hook, AP_CR3_PA);

  int staged = stage_ap_stub(kdldmap(stub_va),
                             cave + KH_OFF_USER + 8, stock_vector);
  if (staged != 0) {
    klog("APE1270,1,stop,stage=carrier,code=%d\n", staged);
    save_line("stop,stage=carrier\n", 0);
    carrier1270_release(&owned_carrier);
    kernel_set_ucred_authid(getpid(), authid);
    return 8;
  }
  if (save_line("carrier,staged,bytes=42,stack=page+0xf00,original_zero=1\n", 0) != 0) {
    int restored = restore_ap_stub(kdldmap(stub_va));
    klog("APE1270,1,stop,stage=save-carrier,restore=%d\n", restored);
    carrier1270_release(&owned_carrier);
    kernel_set_ucred_authid(getpid(), authid);
    return 8;
  }

  klog("APE1270,1,start,target=0x%016" PRIx64
       ",vector=0x%016" PRIx64 ",ap_cr3=0x%016" PRIx64
       ",stub=0x%016" PRIx64 "\n",
       target, stock_vector, (uint64_t)AP_CR3_PA, stub_va);
  if (save_line("hook,before-install\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 0, 1);
    return 9;
  }
  result = kh_install(&hook, NULL);
  if (result != 0) {
    klog("APE1270,1,stop,stage=install,result=%d\n", result);
    save_line("hook,install-failed\n", 0);
    cleanup(&hook, 0, cave, authid, 1, 1);
    return 10;
  }
  if (save_line("hook,after-install\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 1, 1);
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
    cleanup(&hook, 0, cave, authid, 1, 1);
    return 12;
  }

#ifdef AP_ENTRY_PREP_ONLY_1270
  int prep_cleanup = cleanup(&hook, 0, cave, authid, 1, 1);
  save_line(prep_cleanup == 0 ? "end,prepare-only=ok\n" :
            "end,prepare-only=cleanup-failed\n", 0);
  klog("APE1270,1,end,prepare-only,cleanup=%d\n", prep_cleanup);
  return prep_cleanup == 0 ? 0 : 21;
#endif

  if (save_line("hook,before-trigger\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 1, 1);
    return 13;
  }
  result = kh_trigger(&hook);
  if (result != 0) {
    save_line("hook,trigger-failed\n", 0);
    cleanup(&hook, 1, cave, authid, 1, 1);
    return 14;
  }
  if (save_line("hook,after-trigger\n", 0) != 0) {
    cleanup(&hook, 1, cave, authid, 1, 1);
    return 15;
  }

  uint64_t marker = 0, ap_marker = 0;
  if (read_qword(cave + KH_OFF_USER, &marker) != 0 || marker != 0 ||
      read_qword(cave + KH_OFF_USER + 8, &ap_marker) != 0 || ap_marker != 0) {
    save_line("hook,unexpected-early-marker\n", 0);
    cleanup(&hook, 1, cave, authid, 1, 1);
    return 16;
  }

  if (save_line("ready,enter-rest-mode\n", 0) != 0) {
    cleanup(&hook, 1, cave, authid, 1, 1);
    return 17;
  }
  klog("APE1270,1,ready,path=%s\n", output_path);
  notifyf("12.70 AP-entry probe ready\nEnter rest mode, then resume");

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
        klog("APE1270,1,resume-gap,seconds=%ld\n", (long)gap);
      }
      previous = now;
    }
    if (read_qword(cave + KH_OFF_USER, &marker) != 0 ||
        read_qword(cave + KH_OFF_USER + 8, &ap_marker) != 0) {
      save_line_retry("stop,marker-read-failed\n");
      cleanup(&hook, 1, cave, authid, 1, 1);
      return 18;
    }
    if ((marker != 0 && marker != SECOND_MARKER) ||
        (ap_marker != 0 && ap_marker != AP_MARKER)) {
      save_line_retry("stop,unexpected-marker\n");
      cleanup(&hook, 1, cave, authid, 1, 1);
      return 19;
    }
    if (marker == SECOND_MARKER && ap_marker == AP_MARKER) {
      sleep(2); // Let the AP finish its jump to the stock vector.
      break;
    }
    if (marker == SECOND_MARKER && ++resumed_polls >= MAX_RESUME_POLLS)
      break;
  }

  int fired = marker == SECOND_MARKER;
  int ap_entered = ap_marker == AP_MARKER;
  int cleanup_result = cleanup(&hook, !fired, cave, authid, 1, 1);
  snprintf(line, sizeof(line),
           "result,bp=0x%016" PRIx64 ",ap=0x%016" PRIx64
           ",bp_fired=%d,ap_entered=%d,resume_gap=%d,cleanup=%d\n",
           marker, ap_marker, fired, ap_entered, saw_resume_gap,
           cleanup_result);
  int saved = save_line_retry(line);
  klog("APE1270,1,%s", line);
  notifyf("12.70 AP-entry probe\n%s",
          ap_entered ? "AP entry reached" : "no AP marker");
  return fired && ap_entered && cleanup_result == 0 && saved == 0 ? 0 : 20;
}
