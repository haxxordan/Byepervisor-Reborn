#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ps5/kernel.h>

/* This probe caused a KP on 12.70. Retained for diagnosis; do not rerun. */

#define FW_1270 0x12700000U
#define PAGE_SIZE 0x1000U

static unsigned char page[PAGE_SIZE];

static int probe(const char *name, uint64_t va)
{
  memset(page, 0xa5, sizeof(page));
  printf("KTXTREAD1270,1,before,name=%s,va=0x%016" PRIx64 "\n", name, va);
  fflush(stdout);

  int result = kernel_copyout((intptr_t)va, page, sizeof(page));
  unsigned ff = 0, a5 = 0, zero = 0;
  for (unsigned i = 0; i < sizeof(page); ++i) {
    ff += page[i] == 0xff;
    a5 += page[i] == 0xa5;
    zero += page[i] == 0;
  }
  printf("KTXTREAD1270,1,after,name=%s,result=%d,ff=%u,a5=%u,zero=%u,first16=",
         name, result, ff, a5, zero);
  for (unsigned i = 0; i < 16; ++i)
    printf("%02x", page[i]);
  printf("\n");
  fflush(stdout);
  return result == 0 && ff != PAGE_SIZE && a5 != PAGE_SIZE;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  printf("KTXTREAD1270,1,start,fw=0x%08x,text=0x%016" PRIx64
         ",data=0x%016" PRIx64 "\n", fw, text, data);
  fflush(stdout);
  if ((fw & 0xffff0000U) != FW_1270 || !text || data - text != 0xd50000ULL)
    return 1;

  if (!probe("data-control", data))
    return 2;
  if (!probe("text-base", text)) {
    printf("KTXTREAD1270,1,end,stop=text-unreadable\n");
    fflush(stdout);
    return 0;
  }
  if (!probe("pivot", (text + 0x2a815eULL) & ~0xfffULL))
    return 3;
  if (!probe("dbreg-resume", (text + 0x2ac9f8ULL) & ~0xfffULL))
    return 4;
  printf("KTXTREAD1270,1,end,ok\n");
  fflush(stdout);
  return 0;
}
