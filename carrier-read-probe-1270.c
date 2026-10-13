#include <inttypes.h>
#include <stdint.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270 0x12700000U
#define CARRIER_PA 0x3ffff000ULL
#define PAGE_BYTES 0x1000U

static uint8_t page[PAGE_BYTES];

static uint64_t hash_page(const uint8_t *data)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned i = 0; i < PAGE_BYTES; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data <= text || data - text != 0xd50000ULL) {
    klog_printf("CARR1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 1;
  }

  uint64_t pmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t pml4 = 0, cr3 = 0;
  if (kernel_copyout((intptr_t)(pmap + 0x20), &pml4, 8) != 0 ||
      kernel_copyout((intptr_t)(pmap + 0x28), &cr3, 8) != 0 ||
      !pml4 || !cr3 || pml4 <= cr3 || (pml4 & 0xfffULL) ||
      (cr3 & 0xfffULL) || cr3 >= 0x1000000000ULL) {
    klog_printf("CARR1270,1,stop,stage=pmap\n");
    return 1;
  }

  uint64_t dmap = pml4 - cr3;
  uint64_t carrier = dmap + CARRIER_PA;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL) ||
      kernel_copyout((intptr_t)carrier, page, sizeof(page)) != 0) {
    klog_printf("CARR1270,1,stop,stage=read,dmap=0x%016" PRIx64 "\n", dmap);
    return 1;
  }

  unsigned zero = 0, ff = 0;
  for (unsigned i = 0; i < PAGE_BYTES; ++i) {
    zero += page[i] == 0;
    ff += page[i] == 0xff;
  }
  uint64_t first = 0, last = 0;
  for (unsigned i = 0; i < 8; ++i) {
    first |= (uint64_t)page[i] << (i * 8);
    last |= (uint64_t)page[PAGE_BYTES - 8 + i] << (i * 8);
  }
  klog_printf("CARR1270,1,page,va=0x%016" PRIx64
              ",hash=0x%016" PRIx64 ",zero=%u,ff=%u"
              ",first=0x%016" PRIx64 ",last=0x%016" PRIx64 "\n",
              carrier, hash_page(page), zero, ff, first, last);
  klog_printf("CARR1270,1,end,ok\n");
  return 0;
}
