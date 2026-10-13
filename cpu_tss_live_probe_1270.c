#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/cpuset.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

extern int sceKernelGetCurrentCpu(void);

#define FW_1270           0x12700000U
#define SYSTEM_AUTHID     0x4800000000000007ULL
#define PCPU_ARRAY_OFFSET 0x2e9cf00ULL
#define TSS_ARRAY_OFFSET  0x2e8afe0ULL
#define PCPU_STRIDE       0x980ULL
#define TSS_STRIDE        0x68ULL

static int read_qword(uint64_t addr, uint64_t *value)
{
  *value = 0;
  return kernel_copyout((intptr_t)addr, value, sizeof(*value));
}

static void inspect_mask(unsigned affinity_bit, uint64_t pcpu_base,
                         uint64_t tss_base)
{
  uint64_t mask[2] = {1ULL << affinity_bit, 0};
  int set_result = cpuset_setaffinity(3, 1, -1, sizeof(mask),
                                     (const cpuset_t *)mask);
  if (set_result != 0) {
    klog_printf("CTSSLIVE1270,1,affinity,bit=%u,result=%d\n",
                affinity_bit, set_result);
    return;
  }

  int observed_cpu = sceKernelGetCurrentCpu();
  intptr_t current_thread = kernel_get_proc_thread(getpid(), -1);
  klog_printf("CTSSLIVE1270,1,affinity,bit=%u,result=0,observed=%d,"
              "thread=0x%016" PRIx64 "\n",
              affinity_bit, observed_cpu, (uint64_t)current_thread);
  if (!current_thread)
    return;

  unsigned matches = 0;
  for (unsigned row = 0; row < 16; row++) {
    uint64_t pcpu_addr = pcpu_base + (uint64_t)row * PCPU_STRIDE;
    uint64_t active_thread = 0;
    if (read_qword(pcpu_addr, &active_thread) != 0) {
      klog_printf("CTSSLIVE1270,1,stop,stage=pcpu-read,row=%u\n", row);
      return;
    }
    if (active_thread != (uint64_t)current_thread)
      continue;

    uint64_t active_tss = 0, common_tss = 0;
    int tss_read = read_qword(pcpu_addr + 0x790, &active_tss);
    int common_read = read_qword(pcpu_addr + 0x798, &common_tss);
    int active_row = -1, common_row = -1;
    if (active_tss >= tss_base && active_tss < tss_base + 16 * TSS_STRIDE &&
        (active_tss - tss_base) % TSS_STRIDE == 0)
      active_row = (int)((active_tss - tss_base) / TSS_STRIDE);
    if (common_tss >= tss_base && common_tss < tss_base + 16 * TSS_STRIDE &&
        (common_tss - tss_base) % TSS_STRIDE == 0)
      common_row = (int)((common_tss - tss_base) / TSS_STRIDE);
    klog_printf("CTSSLIVE1270,1,match,bit=%u,observed=%d,pcpu_row=%u,"
                "active_tss_row=%d,common_tss_row=%d,read=%d/%d\n",
                affinity_bit, observed_cpu, row, active_row, common_row,
                tss_read, common_read);
    matches++;
  }
  klog_printf("CTSSLIVE1270,1,scan,bit=%u,matches=%u\n",
              affinity_bit, matches);
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog_printf("CTSSLIVE1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 2;
  }

  uint64_t original_authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), SYSTEM_AUTHID) != 0) {
    klog_printf("CTSSLIVE1270,1,stop,stage=authid\n");
    return 3;
  }

  cpusetid_t new_id;
  if (cpuset(&new_id) != 0) {
    klog_printf("CTSSLIVE1270,1,stop,stage=cpuset\n");
    kernel_set_ucred_authid(getpid(), original_authid);
    return 4;
  }
  uint64_t all_mask[2] = {0xffff, 0};
  int allow_result = cpuset_setaffinity(3, 3, new_id, 8,
                                        (const cpuset_t *)all_mask);
  klog_printf("CTSSLIVE1270,1,start,fw=0x%08x,allow_result=%d\n",
              fw, allow_result);
  if (allow_result != 0) {
    kernel_set_ucred_authid(getpid(), original_authid);
    return 5;
  }

  uint64_t data_base = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  inspect_mask(15, data_base + PCPU_ARRAY_OFFSET,
               data_base + TSS_ARRAY_OFFSET);
  inspect_mask(0, data_base + PCPU_ARRAY_OFFSET,
               data_base + TSS_ARRAY_OFFSET);

  int restore_affinity = cpuset_setaffinity(3, 1, -1, sizeof(all_mask),
                                           (const cpuset_t *)all_mask);
  int restore_authid = kernel_set_ucred_authid(getpid(), original_authid);
  klog_printf("CTSSLIVE1270,1,end,affinity_restore=%d,authid_restore=%d\n",
              restore_affinity, restore_authid);
  return restore_affinity == 0 && restore_authid == 0 ? 0 : 6;
}
