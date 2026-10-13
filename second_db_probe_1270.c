#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "khook.h"
#include "util.h"

#define FW_1270              0x12700000U
#define SYSTEM_AUTHID        0x4800000000000007ULL
#define SYSENTS_DATA_OFFSET  0x1af4d0ULL
#define SYSENTS_PS4_OFFSET   0x1a6f80ULL
#define SYS_GETUID           24ULL
#define SYSENT_SIZE          48ULL
#define SYSENT_CALL_OFFSET   8ULL
#define PCPU_ARRAY_OFFSET    0x2e9cf00ULL
#define PCPU_STRIDE          0x980ULL
#define HOOK_CORE            0
#define HOOK_IST             6
#define CAVE_STATE_OFF       0x600ULL
#define BIG_SAVE_BYTES       0x600U
#define SECOND_MARKER        0x1270DBDB5EEDBEEFULL

static uint8_t saved_head[KH_SCRATCH_MIN];
static uint8_t saved_big[BIG_SAVE_BYTES];

static int read_qword(uint64_t addr, uint64_t *value)
{
  *value = 0;
  return kread(addr, value, sizeof(*value));
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog("SDB1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 2;
  }

  uint64_t cave = kdlsym(KERNEL_SYM_DATA_CAVE);
  uint64_t state = 0, marker_before = 0;
  uint64_t entry = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                   SYSENTS_DATA_OFFSET + SYS_GETUID * SYSENT_SIZE +
                   SYSENT_CALL_OFFSET;
  uint64_t ps4_entry = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                       SYSENTS_PS4_OFFSET + SYS_GETUID * SYSENT_SIZE +
                       SYSENT_CALL_OFFSET;
  uint64_t target = 0, ps4_target = 0;
  if (read_qword(cave + CAVE_STATE_OFF, &state) != 0 || state != 0 ||
      read_qword(cave + KH_OFF_USER, &marker_before) != 0 ||
      marker_before != 0 || read_qword(entry, &target) != 0 ||
      read_qword(ps4_entry, &ps4_target) != 0 || ps4_target != target ||
      target < ktext(0) || target >= ktext(KH_KIMG_MAX)) {
    klog("SDB1270,1,stop,stage=preflight,state=0x%016" PRIx64
         ",marker=0x%016" PRIx64 ",target=0x%016" PRIx64
         ",ps4_target=0x%016" PRIx64 "\n",
         state, marker_before, target, ps4_target);
    return 3;
  }

  uint64_t authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), SYSTEM_AUTHID) != 0 ||
      pin_to_core(HOOK_CORE) != 0) {
    klog("SDB1270,1,stop,stage=authid-or-affinity\n");
    kernel_set_ucred_authid(getpid(), authid);
    return 4;
  }

  intptr_t thread = kernel_get_proc_thread(getpid(), -1);
  uint64_t row_thread = 0;
  uint64_t pcpu_row = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                      PCPU_ARRAY_OFFSET + HOOK_CORE * PCPU_STRIDE;
  if (!thread || read_qword(pcpu_row, &row_thread) != 0 ||
      row_thread != (uint64_t)thread) {
    klog("SDB1270,1,stop,stage=thread-row,thread=0x%016" PRIx64
         ",row=0x%016" PRIx64 "\n", (uint64_t)thread, row_thread);
    kernel_set_ucred_authid(getpid(), authid);
    return 5;
  }

  if (kread(cave, saved_head, sizeof(saved_head)) != 0 ||
      kread(cave + KH_OFF_BIG_CHAIN, saved_big, sizeof(saved_big)) != 0) {
    klog("SDB1270,1,stop,stage=backup\n");
    kernel_set_ucred_authid(getpid(), authid);
    return 6;
  }

  khook_t hook;
  int result = kh_init(&hook, kread, kwrite, ktext(0), cave, HOOK_IST);
  if (result != 0) {
    klog("SDB1270,1,stop,stage=kh-init,result=%d\n", result);
    kernel_set_ucred_authid(getpid(), authid);
    return 7;
  }
  hook.tss_stride = KH_TSS_STRIDE;
  kh_set_tables(&hook, kdlsym(KERNEL_SYM_IDT_ARRAY),
                kdlsym(KERNEL_SYM_COMMON_TSS));
  kh_set_core(&hook, HOOK_CORE);
  kh_set_hook_target(&hook, target);
  hook.oneshot = 1;

  int uid_before = getuid();
  klog("SDB1270,1,start,fw=0x%08x,target=0x%016" PRIx64
       ",target_offset=0x%016" PRIx64 ",uid_before=%d\n",
       fw, target, target - ktext(0), uid_before);
  result = kh_install(&hook, NULL);
  if (result != 0) {
    klog("SDB1270,1,stop,stage=kh-install,result=%d\n", result);
    kernel_set_ucred_authid(getpid(), authid);
    return 8;
  }

  uint64_t marker_chain[5] = {0};
  int chain_read = kread(cave + KH_OFF_BIG_CHAIN, marker_chain,
                         sizeof(marker_chain));
  int chain_match = chain_read == 0 &&
                    marker_chain[0] == kgadget(KERNEL_GADGET_POP_RCX) &&
                    marker_chain[1] == cave + KH_OFF_USER &&
                    marker_chain[2] == kgadget(KERNEL_GADGET_POP_RAX) &&
                    marker_chain[3] == SECOND_MARKER &&
                    marker_chain[4] == kgadget(KERNEL_GADGET_ST_RCX_RAX);
  klog("SDB1270,1,installed,chain_read=%d,chain_match=%d\n",
       chain_read, chain_match);
  if (!chain_match)
    result = 9;

#ifndef SECOND_DB_PREP_ONLY_1270
  uint64_t marker_after = 0;
  int marker_read = -1;
  int uid_after = -1;
  int disarm = 0;
  if (result == 0) {
    klog("SDB1270,1,before-trigger\n");
    result = kh_trigger(&hook);
    if (result == 0) {
      klog("SDB1270,1,after-trigger\n");
      klog("SDB1270,1,before-getuid\n");
      uid_after = getuid();
      marker_read = read_qword(cave + KH_OFF_USER, &marker_after);
      klog("SDB1270,1,after-getuid,uid=%d,marker_read=%d,"
           "marker=0x%016" PRIx64 "\n",
           uid_after, marker_read, marker_after);
    }
    if (result != 0 || marker_read != 0 || marker_after != SECOND_MARKER) {
      disarm = kh_disarm(&hook);
      klog("SDB1270,1,disarm,result=%d\n", disarm);
      if (disarm != 0) {
        klog("SDB1270,1,stop,stage=disarm-failed,hook-left-installed\n");
        kernel_set_ucred_authid(getpid(), authid);
        return 12;
      }
    }
  }
#endif

  int hook_restore = kh_restore(&hook);
  klog("SDB1270,1,restore,hook=%d\n", hook_restore);
  if (hook_restore != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    return 10;
  }
  int head_restore = kwrite(cave, saved_head, sizeof(saved_head));
  int big_restore = kwrite(cave + KH_OFF_BIG_CHAIN, saved_big,
                           sizeof(saved_big));
  int auth_restore = kernel_set_ucred_authid(getpid(), authid);
#ifdef SECOND_DB_PREP_ONLY_1270
  int ok = result == 0 && head_restore == 0 && big_restore == 0 &&
           auth_restore == 0;
  klog("SDB1270,1,end,%s,prepare_only=1,head=%d,big=%d,auth=%d\n",
       ok ? "ok" : "fail", head_restore, big_restore, auth_restore);
#else
  int ok = result == 0 && uid_after == uid_before && marker_read == 0 &&
           marker_after == SECOND_MARKER && disarm == 0 &&
           head_restore == 0 && big_restore == 0 && auth_restore == 0;
  klog("SDB1270,1,end,%s,trigger=1,head=%d,big=%d,auth=%d\n",
       ok ? "ok" : "fail", head_restore, big_restore, auth_restore);
#endif
  return ok ? 0 : 11;
}
