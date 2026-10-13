#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270 0x12700000U
#define RANGE_END 0x40000000ULL
#define MAX_SAFE_PA 0x400000000ULL
#define ALLOC_SIZE 0x200000U
#define MAP_NO_COALESCE 0x00400000
#define FRAME_4K 0x000FFFFFFFFFF000ULL
#define FRAME_2M 0x000FFFFFFFE00000ULL
#define FRAME_1G 0x000FFFFFC0000000ULL
#define PRESENT 1ULL
#define LARGE 0x80ULL

typedef int (*fn_alloc_dmem)(off_t, off_t, size_t, size_t, int, off_t *);
typedef int (*fn_map_dmem)(void **, size_t, int, int, off_t, size_t);
typedef int (*fn_release_dmem)(off_t, size_t);

static int read64(uint64_t va, uint64_t *out)
{
  *out = 0;
  return kernel_copyout((intptr_t)va, out, sizeof(*out));
}

static int kernel_ptr(uint64_t va)
{
  return (va >> 48) == 0xffffULL && (va & 7ULL) == 0;
}

static int next_table(uint64_t dmap, uint64_t entry, uint64_t *out)
{
  uint64_t pa = entry & FRAME_4K;
  if (!(entry & PRESENT) || (entry & LARGE) ||
      pa < 0x1000 || pa >= MAX_SAFE_PA)
    return -1;
  *out = dmap + pa;
  return 0;
}

static int translate(uint64_t dmap, uint64_t pml4, uint64_t va,
                     uint64_t *pa, unsigned *leaf_kb)
{
  uint64_t table = pml4, entry = 0;
  const unsigned shifts[] = {39, 30, 21, 12};
  for (unsigned level = 0; level < 4; ++level) {
    if (read64(table + (((va >> shifts[level]) & 0x1ffULL) * 8), &entry) != 0 ||
        !(entry & PRESENT))
      return -1;
    if (level == 1 && (entry & LARGE)) {
      *pa = (entry & FRAME_1G) | (va & 0x3fffffffULL);
      *leaf_kb = 1048576;
      return 0;
    }
    if (level == 2 && (entry & LARGE)) {
      *pa = (entry & FRAME_2M) | (va & 0x1fffffULL);
      *leaf_kb = 2048;
      return 0;
    }
    if (level == 3) {
      *pa = (entry & FRAME_4K) | (va & 0xfffULL);
      *leaf_kb = 4;
      return 0;
    }
    if (next_table(dmap, entry, &table) != 0)
      return -1;
  }
  return -1;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data <= text || data - text != 0xd50000ULL) {
    klog_printf("OWNMAP1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 1;
  }

  fn_alloc_dmem allocate =
      (fn_alloc_dmem)dlsym(RTLD_DEFAULT, "sceKernelAllocateDirectMemory");
  fn_map_dmem map =
      (fn_map_dmem)dlsym(RTLD_DEFAULT, "sceKernelMapDirectMemory");
  fn_release_dmem release =
      (fn_release_dmem)dlsym(RTLD_DEFAULT, "sceKernelReleaseDirectMemory");
  if (!allocate || !map || !release) {
    klog_printf("OWNMAP1270,1,stop,stage=symbols,alloc=%u,map=%u,release=%u\n",
                !!allocate, !!map, !!release);
    return 1;
  }

  int result = 1, allocated = 0, mapped = 0;
  off_t offset = 0;
  void *buffer = NULL;
  uint64_t pa = 0, dmap = 0;
  unsigned leaf_kb = 0;
  klog_printf("OWNMAP1270,1,start,fw=0x%08x\n", fw);
  int rc = allocate(0, (off_t)RANGE_END, ALLOC_SIZE, ALLOC_SIZE, 0, &offset);
  klog_printf("OWNMAP1270,1,alloc,result=%d,offset=0x%016" PRIx64 "\n",
              rc, (uint64_t)offset);
  if (rc != 0)
    goto done;
  allocated = 1;

  rc = map(&buffer, ALLOC_SIZE, PROT_READ | PROT_WRITE,
           MAP_NO_COALESCE, offset, ALLOC_SIZE);
  klog_printf("OWNMAP1270,1,map,result=%d,va=0x%016" PRIx64 "\n",
              rc, (uint64_t)(uintptr_t)buffer);
  if (rc != 0 || !buffer)
    goto done;
  mapped = 1;

  uint64_t kpmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t kpml4 = 0, kcr3 = 0;
  if (read64(kpmap + 0x20, &kpml4) != 0 ||
      read64(kpmap + 0x28, &kcr3) != 0 ||
      !kernel_ptr(kpml4) || !kcr3 || kpml4 <= kcr3 ||
      (kpml4 & 0xfffULL) || (kcr3 & 0xfffULL)) {
    klog_printf("OWNMAP1270,1,stop,stage=kernel-pmap\n");
    goto done;
  }
  dmap = kpml4 - kcr3;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL)) {
    klog_printf("OWNMAP1270,1,stop,stage=dmap\n");
    goto done;
  }

  uint64_t proc = kernel_get_proc(getpid()), vmspace = 0, upmap = 0, upml4 = 0;
  if (!kernel_ptr(proc) ||
      read64(proc + (uint64_t)KERNEL_OFFSET_PROC_P_VMSPACE,
                      &vmspace) != 0 ||
      !kernel_ptr(vmspace) || read64(vmspace + 0x1d8, &upmap) != 0 ||
      !kernel_ptr(upmap) || read64(upmap + 0x20, &upml4) != 0 ||
      !kernel_ptr(upml4) || (upml4 & 0xfffULL)) {
    klog_printf("OWNMAP1270,1,stop,stage=user-pmap,proc=0x%016" PRIx64
                ",vmspace=0x%016" PRIx64 ",pmap=0x%016" PRIx64 "\n",
                proc, vmspace, upmap);
    goto done;
  }

  if (translate(dmap, upml4, (uint64_t)(uintptr_t)buffer,
                &pa, &leaf_kb) != 0 || pa < 0x1000 ||
      pa >= MAX_SAFE_PA) {
    klog_printf("OWNMAP1270,1,stop,stage=translate,pa=0x%016" PRIx64 "\n", pa);
    goto done;
  }

  uint64_t marker = 0x314e574f30323731ULL, observed = 0;
  memcpy(buffer, &marker, sizeof(marker));
  if (read64(dmap + pa, &observed) != 0 || observed != marker) {
    klog_printf("OWNMAP1270,1,stop,stage=alias,pa=0x%016" PRIx64
                ",observed=0x%016" PRIx64 "\n", pa, observed);
    goto done;
  }
  klog_printf("OWNMAP1270,1,physical,pa=0x%016" PRIx64
              ",leaf_kb=%u,below_1g=%u,alias_match=1\n",
              pa, leaf_kb, (unsigned)(pa < RANGE_END));
  result = 0;

done:
  if (mapped) {
    rc = munmap(buffer, ALLOC_SIZE);
    klog_printf("OWNMAP1270,1,cleanup,munmap=%d\n", rc);
    if (rc != 0)
      result = 1;
  }
  if (allocated) {
    rc = release(offset, ALLOC_SIZE);
    klog_printf("OWNMAP1270,1,cleanup,release=%d\n", rc);
    if (rc != 0)
      result = 1;
  }
  klog_printf("OWNMAP1270,1,end,%s\n", result == 0 ? "ok" : "stop");
  return result;
}
