#include <inttypes.h>
#include <stdint.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270 0x12700000U
#define AP_CR3_PA 0x68000ULL
#define MAX_TABLE_PA 0x400000000ULL
#define FRAME_4K 0x000FFFFFFFFFF000ULL
#define FRAME_2M 0x000FFFFFFFE00000ULL
#define FRAME_1G 0x000FFFFFC0000000ULL
#define NX (1ULL << 63)

struct walk_result {
  uint64_t pa;
  uint64_t pml4e;
  uint64_t pdpte;
  uint64_t pde;
  uint64_t pte;
  unsigned leaf_kb;
  unsigned nx;
};

static int read64(uint64_t va, uint64_t *out)
{
  return kernel_copyout((intptr_t)va, out, sizeof(*out));
}

static int next_table(uint64_t dmap, uint64_t entry, uint64_t *out)
{
  uint64_t pa = entry & FRAME_4K;
  if (!(entry & 1ULL) || (entry & 0x80ULL) ||
      pa < 0x1000 || pa >= MAX_TABLE_PA)
    return -1;
  *out = dmap + pa;
  return 0;
}

static int walk(uint64_t dmap, uint64_t va, struct walk_result *out)
{
  uint64_t table = dmap + AP_CR3_PA;
  if (read64(table + (((va >> 39) & 0x1ffULL) * 8), &out->pml4e) != 0 ||
      next_table(dmap, out->pml4e, &table) != 0)
    return -1;
  out->nx |= !!(out->pml4e & NX);
  if (read64(table + (((va >> 30) & 0x1ffULL) * 8), &out->pdpte) != 0 ||
      !(out->pdpte & 1ULL))
    return -1;
  out->nx |= !!(out->pdpte & NX);
  if (out->pdpte & 0x80ULL) {
    out->pa = (out->pdpte & FRAME_1G) | (va & 0x3fffffffULL);
    out->leaf_kb = 1048576;
    return 0;
  }
  if (next_table(dmap, out->pdpte, &table) != 0 ||
      read64(table + (((va >> 21) & 0x1ffULL) * 8), &out->pde) != 0 ||
      !(out->pde & 1ULL))
    return -1;
  out->nx |= !!(out->pde & NX);
  if (out->pde & 0x80ULL) {
    out->pa = (out->pde & FRAME_2M) | (va & 0x1fffffULL);
    out->leaf_kb = 2048;
    return 0;
  }
  if (next_table(dmap, out->pde, &table) != 0 ||
      read64(table + (((va >> 12) & 0x1ffULL) * 8), &out->pte) != 0 ||
      !(out->pte & 1ULL))
    return -1;
  out->nx |= !!(out->pte & NX);
  out->pa = (out->pte & FRAME_4K) | (va & 0xfffULL);
  out->leaf_kb = 4;
  return 0;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data <= text || data - text != 0xd50000ULL) {
    klog_printf("APCR31270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 1;
  }
  uint64_t pmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t pml4 = 0, cr3 = 0;
  if (read64(pmap + 0x20, &pml4) != 0 ||
      read64(pmap + 0x28, &cr3) != 0 ||
      (pml4 >> 48) != 0xffffULL || pml4 <= cr3 ||
      (pml4 & 0xfffULL) || (cr3 & 0xfffULL) ||
      cr3 >= MAX_TABLE_PA) {
    klog_printf("APCR31270,1,stop,stage=pmap\n");
    return 1;
  }
  uint64_t dmap = pml4 - cr3;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL)) {
    klog_printf("APCR31270,1,stop,stage=dmap\n");
    return 1;
  }

  const uint64_t samples[] = {
    0x0000000000068000ULL,
    0x0000000001000000ULL,
    0x0000000026658000ULL,
    0x000000003fe06000ULL,
    0x000000003ffff000ULL,
#ifdef AP_CR3_HIGH_1270
    text + g_sym_map_1270[KERNEL_SYM_MP_STARTUP_INIT_VECTOR],
    text + g_sym_map_1270[KERNEL_SYM_DATA_CAVE],
    dmap + 0x3ffff100ULL,
#endif
  };
  unsigned mapped = 0, identity_exec = 0;
  klog_printf("APCR31270,1,start,fw=0x%08x,cr3_pa=0x%016" PRIx64
              ",samples=%u\n", fw, AP_CR3_PA,
              (unsigned)(sizeof(samples) / sizeof(samples[0])));
  for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    struct walk_result result = {0};
    int rc = walk(dmap, samples[i], &result);
    mapped += rc == 0;
    identity_exec += rc == 0 && result.pa == samples[i] && !result.nx;
    klog_printf("APCR31270,1,walk,index=%u,va=0x%016" PRIx64
                ",result=%d,pa=0x%016" PRIx64 ",leaf_kb=%u,nx=%u"
                ",pml4e=0x%016" PRIx64 ",pdpte=0x%016" PRIx64
                ",pde=0x%016" PRIx64 ",pte=0x%016" PRIx64 "\n",
                i, samples[i], rc, result.pa, result.leaf_kb, result.nx,
                result.pml4e, result.pdpte, result.pde, result.pte);
  }
  klog_printf("APCR31270,1,end,ok,mapped=%u,identity_exec=%u,total=%u\n",
              mapped, identity_exec,
              (unsigned)(sizeof(samples) / sizeof(samples[0])));
  return 0;
}
