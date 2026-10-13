#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define FW_1270              0x12700000U
#define SYSENTS_DATA_OFFSET  0x1af4d0ULL
#define SYSENTS_PS4_OFFSET   0x1a6f80ULL
#define SYS_GETPID           20ULL
#define SYS_GETUID           24ULL
#define SYSENT_SIZE          48ULL
#define SYSENT_CALL_OFFSET   8ULL
#define CANDIDATE_GETUID_OFF 0x3573b0ULL

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog_printf("SYSGU1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 2;
  }

  uint64_t candidate = (uint64_t)KERNEL_ADDRESS_TEXT_BASE +
                       CANDIDATE_GETUID_OFF;
  uint64_t tables[] = {SYSENTS_DATA_OFFSET, SYSENTS_PS4_OFFSET};
  uint64_t calls[] = {SYS_GETPID, SYS_GETUID};
  unsigned valid = 0;
  klog_printf("SYSGU1270,1,start,text=0x%016" PRIx64
              ",data=0x%016" PRIx64 ",candidate=0x%016" PRIx64 "\n",
              (uint64_t)KERNEL_ADDRESS_TEXT_BASE,
              (uint64_t)KERNEL_ADDRESS_DATA_BASE, candidate);
  for (unsigned t = 0; t < 2; ++t) {
    for (unsigned c = 0; c < 2; ++c) {
      uint64_t entry = (uint64_t)KERNEL_ADDRESS_DATA_BASE + tables[t] +
                       calls[c] * SYSENT_SIZE + SYSENT_CALL_OFFSET;
      uint64_t actual = 0;
      int read_result = kernel_copyout((intptr_t)entry, &actual,
                                       sizeof(actual));
      if (read_result == 0)
        ++valid;
      klog_printf("SYSGU1270,1,row,table=%s,sys=%s,entry=0x%016" PRIx64
                  ",actual=0x%016" PRIx64 ",text_offset=0x%016" PRIx64
                  ",read=%d,candidate_match=%d\n",
                  t ? "ps4" : "native", c ? "getuid" : "getpid",
                  entry, actual,
                  actual >= (uint64_t)KERNEL_ADDRESS_TEXT_BASE ?
                    actual - (uint64_t)KERNEL_ADDRESS_TEXT_BASE : 0,
                  read_result, read_result == 0 && actual == candidate);
    }
  }
  klog_printf("SYSGU1270,1,end,valid=%u,total=4\n", valid);
  return valid == 4 ? 0 : 3;
}
