#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

extern int sceKernelGetCurrentCpu(void);

#define FW_1270          0x12700000U
#define PCPU_ARRAY_OFFSET 0x2e9cf00ULL
#define TSS_ARRAY_OFFSET  0x2e8afe0ULL
#define PCPU_STRIDE       0x980ULL
#define TSS_STRIDE        0x68ULL
#define PCPU_BYTES        0x980U

static uint8_t pcpu_snapshot[PCPU_BYTES];

static int inspect_cpu(unsigned cpu, uint64_t pcpu_base, uint64_t tss_base)
{
  uint64_t addr = pcpu_base + (uint64_t)cpu * PCPU_STRIDE;
  klog_printf("CTSS1270,1,before-read,cpu=%u,va=0x%016" PRIx64 "\n",
              cpu, addr);
  int result = kernel_copyout((intptr_t)addr, pcpu_snapshot,
                              sizeof(pcpu_snapshot));
  if (result != 0) {
    klog_printf("CTSS1270,1,read,cpu=%u,result=%d\n", cpu, result);
    return -1;
  }

  uint32_t cpuid_field = 0;
  for (unsigned byte = 0; byte < 4; byte++)
    cpuid_field |= (uint32_t)pcpu_snapshot[0x34 + byte] << (8 * byte);
  klog_printf("CTSS1270,1,identity,row=%u,pc_cpuid=%u\n", cpu, cpuid_field);

  unsigned matches = 0;
  for (unsigned offset = 0; offset + 8 <= sizeof(pcpu_snapshot); offset += 8) {
    uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; byte++)
      value |= (uint64_t)pcpu_snapshot[offset + byte] << (8 * byte);
    if (value < tss_base || value >= tss_base + 16 * TSS_STRIDE)
      continue;
    uint64_t delta = value - tss_base;
    if (delta % TSS_STRIDE != 0)
      continue;
    klog_printf("CTSS1270,1,match,cpu=%u,pcpu_offset=0x%x,tss_row=%u,"
                "pointer=0x%016" PRIx64 "\n",
                cpu, offset, (unsigned)(delta / TSS_STRIDE), value);
    matches++;
  }
  klog_printf("CTSS1270,1,row,cpu=%u,read=ok,matches=%u\n", cpu, matches);
  return 0;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog_printf("CTSS1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 2;
  }

  uint64_t data_base = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  uint64_t pcpu_base = data_base + PCPU_ARRAY_OFFSET;
  uint64_t tss_base = data_base + TSS_ARRAY_OFFSET;
  klog_printf("CTSS1270,1,start,fw=0x%08x,current_cpu=%d,"
              "pcpu=0x%016" PRIx64 ",tss=0x%016" PRIx64 "\n",
              fw, sceKernelGetCurrentCpu(), pcpu_base, tss_base);

  int current_cpu = sceKernelGetCurrentCpu();
  if (inspect_cpu(0, pcpu_base, tss_base) != 0 ||
      inspect_cpu(15, pcpu_base, tss_base) != 0 ||
      (current_cpu > 0 && current_cpu < 15 &&
       inspect_cpu((unsigned)current_cpu, pcpu_base, tss_base) != 0)) {
    klog_printf("CTSS1270,1,end,read-error\n");
    return 1;
  }
  klog_printf("CTSS1270,1,end,ok\n");
  return 0;
}
