#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <ps5/kernel.h>

/* This probe caused a KP on 12.70 even with only elfldr resident.
 * Retained for diagnosis; do not run it again unchanged. */

#define FW_1270 0x12700000U

static int probe(const char *name, uint64_t va)
{
  uint64_t value = UINT64_C(0xa5a5a5a5a5a5a5a5);
  printf("KTXTQ1270,1,before,name=%s,va=0x%016" PRIx64 "\n", name, va);
  fflush(stdout);
  int result = kernel_copyout((intptr_t)va, &value, sizeof(value));
  printf("KTXTQ1270,1,after,name=%s,result=%d,value=0x%016" PRIx64 "\n",
         name, result, value);
  fflush(stdout);
  return result;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  printf("KTXTQ1270,1,start,fw=0x%08x,text=0x%016" PRIx64
         ",data=0x%016" PRIx64 "\n", fw, text, data);
  fflush(stdout);
  if ((fw & 0xffff0000U) != FW_1270 || !text || data - text != 0xd50000ULL)
    return 1;

  if (probe("data-control", data) != 0)
    return 2;
  if (probe("text-base", text) != 0)
    return 3;
  printf("KTXTQ1270,1,end,ok\n");
  fflush(stdout);
  return 0;
}
