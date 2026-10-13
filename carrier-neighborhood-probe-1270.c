#include <inttypes.h>
#include <stdint.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270 0x12700000U
#define PA_BEGIN 0x3fe00000ULL
#define PA_END 0x40000000ULL
#define PAGE_BYTES 0x1000U
#define TOP_COUNT 16U
#define FRAME_4K 0x000FFFFFFFFFF000ULL
#define FRAME_2M 0x000FFFFFFFE00000ULL

struct candidate {
  uint64_t pa;
  uint64_t hash;
  unsigned nonzero;
};

static uint8_t page[PAGE_BYTES];
static struct candidate least[TOP_COUNT];

static int read64(uint64_t va, uint64_t *out)
{
  return kernel_copyout((intptr_t)va, out, sizeof(*out));
}

static uint64_t hash_page(const uint8_t *data)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned i = 0; i < PAGE_BYTES; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

static void record(uint64_t pa, uint64_t hash, unsigned nonzero)
{
  for (unsigned i = 0; i < TOP_COUNT; ++i) {
    if (nonzero < least[i].nonzero) {
      for (unsigned j = TOP_COUNT - 1; j > i; --j)
        least[j] = least[j - 1];
      least[i].pa = pa;
      least[i].hash = hash;
      least[i].nonzero = nonzero;
      return;
    }
  }
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data <= text || data - text != 0xd50000ULL) {
    klog_printf("CARRSCAN1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 1;
  }

  uint64_t pmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t pml4 = 0, cr3 = 0;
  if (kernel_copyout((intptr_t)(pmap + 0x20), &pml4, 8) != 0 ||
      kernel_copyout((intptr_t)(pmap + 0x28), &cr3, 8) != 0 ||
      !pml4 || !cr3 || pml4 <= cr3 || (pml4 & 0xfffULL) ||
      (cr3 & 0xfffULL) || cr3 >= 0x1000000000ULL) {
    klog_printf("CARRSCAN1270,1,stop,stage=pmap\n");
    return 1;
  }

  uint64_t dmap = pml4 - cr3;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL)) {
    klog_printf("CARRSCAN1270,1,stop,stage=dmap\n");
    return 1;
  }

  uint64_t va = dmap + PA_BEGIN, pml4e = 0, pdpte = 0, pde = 0;
  if (read64(pml4 + (((va >> 39) & 0x1ffULL) * 8), &pml4e) != 0 ||
      !(pml4e & 1ULL) || (pml4e & 0x80ULL) ||
      read64(dmap + (pml4e & FRAME_4K) +
             (((va >> 30) & 0x1ffULL) * 8), &pdpte) != 0 ||
      !(pdpte & 1ULL) || (pdpte & 0x80ULL) ||
      read64(dmap + (pdpte & FRAME_4K) +
             (((va >> 21) & 0x1ffULL) * 8), &pde) != 0 ||
      !(pde & 1ULL) || !(pde & 0x80ULL) ||
      (pde & FRAME_2M) != PA_BEGIN) {
    klog_printf("CARRSCAN1270,1,stop,stage=leaf,pde=0x%016" PRIx64 "\n",
                pde);
    return 1;
  }

  for (unsigned i = 0; i < TOP_COUNT; ++i)
    least[i].nonzero = PAGE_BYTES + 1;

  unsigned pages = 0, zero_pages = 0, sparse_pages = 0;
  uint64_t carrier_hash = 0;
  unsigned carrier_nonzero = 0;
  klog_printf("CARRSCAN1270,1,start,fw=0x%08x,begin=0x%016" PRIx64
              ",end=0x%016" PRIx64 "\n", fw, PA_BEGIN, PA_END);
  for (uint64_t pa = PA_BEGIN; pa < PA_END; pa += PAGE_BYTES) {
    if (kernel_copyout((intptr_t)(dmap + pa), page, PAGE_BYTES) != 0) {
      klog_printf("CARRSCAN1270,1,stop,stage=read,pa=0x%016" PRIx64
                  ",pages=%u\n", pa, pages);
      return 1;
    }
    unsigned nonzero = 0;
    for (unsigned j = 0; j < PAGE_BYTES; ++j)
      nonzero += page[j] != 0;
    uint64_t hash = hash_page(page);
    zero_pages += nonzero == 0;
    sparse_pages += nonzero <= 64;
    record(pa, hash, nonzero);
    if (pa == 0x3ffff000ULL) {
      carrier_hash = hash;
      carrier_nonzero = nonzero;
    }
    ++pages;
  }

  klog_printf("CARRSCAN1270,1,summary,pages=%u,zero=%u,sparse_le64=%u"
              ",carrier_nonzero=%u,carrier_hash=0x%016" PRIx64 "\n",
              pages, zero_pages, sparse_pages, carrier_nonzero, carrier_hash);
  for (unsigned i = 0; i < TOP_COUNT; ++i) {
    if (least[i].nonzero > PAGE_BYTES)
      break;
    klog_printf("CARRSCAN1270,1,least,index=%u,pa=0x%016" PRIx64
                ",nonzero=%u,hash=0x%016" PRIx64 "\n",
                i, least[i].pa, least[i].nonzero, least[i].hash);
  }
  klog_printf("CARRSCAN1270,1,end,ok\n");
  return 0;
}
