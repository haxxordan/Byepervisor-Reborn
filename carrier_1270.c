#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "carrier_1270.h"

#define ALLOC_BYTES 0x400000U
#define PAGE_BYTES 0x1000U
#define LOW_LIMIT 0x40000000ULL
#define RAM_LIMIT 0x400000000ULL
#define FRAME_4K 0x000FFFFFFFFFF000ULL
#define FRAME_2M 0x000FFFFFFFE00000ULL
#define FRAME_1G 0x000FFFFFC0000000ULL

static int read64(uint64_t va, uint64_t *out)
{
  return kernel_copyout((intptr_t)va, out, sizeof(*out));
}

static int kernel_ptr(uint64_t va)
{
  return (va >> 48) == 0xffffULL && (va & 7ULL) == 0;
}

static int translate(uint64_t dmap, uint64_t pml4, uint64_t va,
                     uint64_t *pa)
{
  uint64_t table = pml4, entry = 0;
  const unsigned shifts[] = {39, 30, 21, 12};
  for (unsigned level = 0; level < 4; ++level) {
    if (read64(table + (((va >> shifts[level]) & 0x1ffULL) * 8), &entry) != 0 ||
        !(entry & 1ULL))
      return -1;
    if (level == 1 && (entry & 0x80ULL)) {
      *pa = (entry & FRAME_1G) | (va & 0x3fffffffULL);
      return 0;
    }
    if (level == 2 && (entry & 0x80ULL)) {
      *pa = (entry & FRAME_2M) | (va & 0x1fffffULL);
      return 0;
    }
    if (level == 3) {
      *pa = (entry & FRAME_4K) | (va & 0xfffULL);
      return 0;
    }
    uint64_t table_pa = entry & FRAME_4K;
    if ((entry & 0x80ULL) || table_pa < 0x1000 || table_pa >= RAM_LIMIT)
      return -1;
    table = dmap + table_pa;
  }
  return -1;
}

static int user_pml4(uint64_t *pml4)
{
  uint64_t proc = kernel_get_proc(getpid()), vmspace = 0, upmap = 0;
  if (!kernel_ptr(proc) ||
      read64(proc + (uint64_t)KERNEL_OFFSET_PROC_P_VMSPACE, &vmspace) != 0 ||
      !kernel_ptr(vmspace) || read64(vmspace + 0x1d8, &upmap) != 0 ||
      !kernel_ptr(upmap) || read64(upmap + 0x20, pml4) != 0 ||
      !kernel_ptr(*pml4) || (*pml4 & 0xfffULL))
    return -1;
  return 0;
}

static int ap_identity_exec(uint64_t dmap, uint64_t pa)
{
  uint64_t pml4e = 0, pdpte = 0, pde = 0;
  if (read64(dmap + 0x68000ULL, &pml4e) != 0 ||
      !(pml4e & 1ULL) || (pml4e & 0x80ULL) ||
      (pml4e & FRAME_4K) >= RAM_LIMIT ||
      read64(dmap + (pml4e & FRAME_4K), &pdpte) != 0 ||
      !(pdpte & 1ULL) || (pdpte & 0x80ULL) ||
      (pdpte & FRAME_4K) >= RAM_LIMIT ||
      read64(dmap + (pdpte & FRAME_4K) +
             (((pa >> 21) & 0x1ffULL) * 8), &pde) != 0)
    return -1;
  return (pde & 0x81ULL) == 0x81ULL &&
         (pde & FRAME_2M) == (pa & FRAME_2M) &&
         !((pml4e | pdpte | pde) & (1ULL << 63)) ? 0 : -1;
}

int carrier1270_acquire(carrier1270_t *carrier, uint64_t dmap)
{
  if (!carrier || (dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL))
    return -1;
  memset(carrier, 0, sizeof(*carrier));
  carrier->dmap = dmap;
  carrier->mapping = mmap(NULL, ALLOC_BYTES, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (carrier->mapping == MAP_FAILED) {
    carrier->mapping = NULL;
    return -2;
  }
  for (unsigned i = 0; i < ALLOC_BYTES / PAGE_BYTES; ++i)
    carrier->mapping[i * PAGE_BYTES] = (uint8_t)(i ^ 0x5a);

  uint64_t pml4 = 0;
  if (user_pml4(&pml4) != 0)
    goto fail;
  for (unsigned i = 0; i < ALLOC_BYTES / PAGE_BYTES; ++i) {
    uint8_t *page = carrier->mapping + i * PAGE_BYTES;
    uint64_t pa = 0;
    if (translate(dmap, pml4, (uint64_t)(uintptr_t)page, &pa) != 0 ||
        (pa & (PAGE_BYTES - 1)))
      goto fail;
    if (pa >= LOW_LIMIT)
      continue;
    uint8_t alias_byte = 0;
    if (kernel_copyout((intptr_t)(dmap + pa), &alias_byte, 1) != 0 ||
        alias_byte != page[0] || ap_identity_exec(dmap, pa) != 0)
      continue;
    if (mlock(page, PAGE_BYTES) != 0)
      goto fail;
    uint64_t locked_pa = 0;
    if (translate(dmap, pml4, (uint64_t)(uintptr_t)page, &locked_pa) != 0 ||
        locked_pa != pa) {
      munlock(page, PAGE_BYTES);
      goto fail;
    }
    carrier->page = page;
    carrier->pa = pa;
    carrier->locked = 1;
    return 0;
  }
fail:
  carrier1270_release(carrier);
  return -3;
}

int carrier1270_translate(const carrier1270_t *carrier, uint64_t *pa)
{
  uint64_t pml4 = 0;
  if (!carrier || !carrier->page || !pa || user_pml4(&pml4) != 0)
    return -1;
  return translate(carrier->dmap, pml4,
                   (uint64_t)(uintptr_t)carrier->page, pa);
}

int carrier1270_release(carrier1270_t *carrier)
{
  if (!carrier)
    return -1;
  int rc = 0;
  if (carrier->locked && munlock(carrier->page, PAGE_BYTES) != 0)
    rc = -1;
  if (carrier->mapping && munmap(carrier->mapping, ALLOC_BYTES) != 0)
    rc = -1;
  memset(carrier, 0, sizeof(*carrier));
  return rc;
}
