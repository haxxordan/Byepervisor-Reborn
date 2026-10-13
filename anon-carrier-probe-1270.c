#include <inttypes.h>
#include <stdint.h>
#include <sys/mman.h>
#ifdef ANON_HOLD_1270
#include <time.h>
#endif
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270 0x12700000U
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

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data <= text || data - text != 0xd50000ULL) {
    klog_printf("ANONCARR1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 1;
  }

  uint8_t *buffer = mmap(NULL, ALLOC_BYTES, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (buffer == MAP_FAILED) {
    klog_printf("ANONCARR1270,1,stop,stage=mmap\n");
    return 1;
  }

  int result = 1;
  const unsigned total = ALLOC_BYTES / PAGE_BYTES;
  klog_printf("ANONCARR1270,1,start,fw=0x%08x,va=0x%016" PRIx64
              ",bytes=%u,pages=%u\n", fw, (uint64_t)(uintptr_t)buffer,
              ALLOC_BYTES, total);
  for (unsigned i = 0; i < total; ++i)
    buffer[i * PAGE_BYTES] = (uint8_t)(i ^ 0x5a);

  uint64_t kpmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t kpml4 = 0, kcr3 = 0;
  if (read64(kpmap + 0x20, &kpml4) != 0 ||
      read64(kpmap + 0x28, &kcr3) != 0 ||
      !kernel_ptr(kpml4) || !kcr3 || kpml4 <= kcr3 ||
      (kpml4 & 0xfffULL) || (kcr3 & 0xfffULL)) {
    klog_printf("ANONCARR1270,1,stop,stage=kernel-pmap\n");
    goto done;
  }
  uint64_t dmap = kpml4 - kcr3;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL)) {
    klog_printf("ANONCARR1270,1,stop,stage=dmap\n");
    goto done;
  }

  uint64_t proc = kernel_get_proc(getpid()), vmspace = 0, upmap = 0, upml4 = 0;
  if (!kernel_ptr(proc) ||
      read64(proc + (uint64_t)KERNEL_OFFSET_PROC_P_VMSPACE, &vmspace) != 0 ||
      !kernel_ptr(vmspace) || read64(vmspace + 0x1d8, &upmap) != 0 ||
      !kernel_ptr(upmap) || read64(upmap + 0x20, &upml4) != 0 ||
      !kernel_ptr(upml4) || (upml4 & 0xfffULL)) {
    klog_printf("ANONCARR1270,1,stop,stage=user-pmap\n");
    goto done;
  }

  unsigned low = 0, ram = 0, high = 0;
  uint64_t min_pa = UINT64_MAX, max_pa = 0, first_low = 0;
#ifdef ANON_HOLD_1270
  uint8_t *first_low_va = NULL;
#endif
  int alias_match = 0;
  for (unsigned i = 0; i < total; ++i) {
    uint64_t pa = 0;
    if (translate(dmap, upml4,
                  (uint64_t)(uintptr_t)(buffer + i * PAGE_BYTES), &pa) != 0 ||
        (pa & (PAGE_BYTES - 1))) {
      klog_printf("ANONCARR1270,1,stop,stage=translate,index=%u,pa=0x%016" PRIx64 "\n",
                  i, pa);
      goto done;
    }
    if (pa < min_pa) min_pa = pa;
    if (pa > max_pa) max_pa = pa;
    if (pa < LOW_LIMIT) {
      ++low;
      if (first_low == 0) {
        first_low = pa;
#ifdef ANON_HOLD_1270
        first_low_va = buffer + i * PAGE_BYTES;
#endif
        uint8_t observed = 0;
        alias_match = kernel_copyout((intptr_t)(dmap + pa), &observed, 1) == 0 &&
                      observed == buffer[i * PAGE_BYTES];
      }
    } else if (pa < RAM_LIMIT) {
      ++ram;
    } else {
      ++high;
    }
  }
  klog_printf("ANONCARR1270,1,summary,low_1g=%u,mid_16g=%u,above_16g=%u"
              ",min_pa=0x%016" PRIx64 ",max_pa=0x%016" PRIx64
              ",first_low=0x%016" PRIx64 ",alias_match=%d\n",
              low, ram, high, min_pa, max_pa, first_low, alias_match);
  result = 0;

#ifdef ANON_HOLD_1270
  if (!first_low_va || !alias_match) {
    klog_printf("ANONHOLD1270,1,stop,stage=no-low-page\n");
    result = 1;
    goto done;
  }
  int lock_rc = mlock(first_low_va, PAGE_BYTES);
  klog_printf("ANONHOLD1270,1,lock,result=%d,pa=0x%016" PRIx64
              ",va=0x%016" PRIx64 "\n", lock_rc, first_low,
              (uint64_t)(uintptr_t)first_low_va);
  if (lock_rc != 0) {
    result = 1;
    goto done;
  }
  const uint64_t marker = 0x3037323152524143ULL;
  *(volatile uint64_t *)first_low_va = marker;
  uint64_t observed = 0;
  if (read64(dmap + first_low, &observed) != 0 || observed != marker) {
    klog_printf("ANONHOLD1270,1,stop,stage=marker,observed=0x%016" PRIx64
                "\n", observed);
    result = 1;
    goto unlock;
  }
  struct timespec previous = {0};
  if (clock_gettime(CLOCK_REALTIME, &previous) != 0) {
    klog_printf("ANONHOLD1270,1,stop,stage=clock\n");
    result = 1;
    goto unlock;
  }
  klog_printf("ANONHOLD1270,1,ready,pa=0x%016" PRIx64
              ",marker=0x%016" PRIx64 ",wait_seconds=600\n",
              first_low, marker);
  unsigned resume_seen = 0;
  for (unsigned i = 0; i < 600; ++i) {
    sleep(1);
    struct timespec now = {0};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
      klog_printf("ANONHOLD1270,1,stop,stage=clock-loop\n");
      result = 1;
      break;
    }
    int64_t gap = (int64_t)now.tv_sec - (int64_t)previous.tv_sec;
    previous = now;
    if (gap >= 10) {
      uint64_t pa_after = 0, marker_after = 0;
      int walk_rc = translate(dmap, upml4,
                              (uint64_t)(uintptr_t)first_low_va, &pa_after);
      int alias_rc = walk_rc == 0 && pa_after == first_low ?
          read64(dmap + pa_after, &marker_after) : -1;
      klog_printf("ANONHOLD1270,1,resume,gap_seconds=%" PRId64
                  ",walk=%d,pa_before=0x%016" PRIx64
                  ",pa_after=0x%016" PRIx64
                  ",alias_read=%d,marker=0x%016" PRIx64
                  ",match=%d\n", gap, walk_rc, first_low, pa_after,
                  alias_rc, marker_after,
                  alias_rc == 0 && marker_after == marker);
      resume_seen = 1;
      if (walk_rc != 0 || pa_after != first_low ||
          alias_rc != 0 || marker_after != marker)
        result = 1;
      break;
    }
    if ((i + 1) % 30 == 0)
      klog_printf("ANONHOLD1270,1,waiting,seconds=%u\n", i + 1);
  }
  if (!resume_seen && result == 0) {
    klog_printf("ANONHOLD1270,1,timeout,no-resume-observed\n");
    result = 1;
  }
unlock:;
  int unlock_rc = munlock(first_low_va, PAGE_BYTES);
  klog_printf("ANONHOLD1270,1,cleanup,munlock=%d\n", unlock_rc);
  if (unlock_rc != 0)
    result = 1;
#endif

done:;
  int unmap_rc = munmap(buffer, ALLOC_BYTES);
  klog_printf("ANONCARR1270,1,cleanup,munmap=%d\n", unmap_rc);
  if (unmap_rc != 0)
    result = 1;
  klog_printf("ANONCARR1270,1,end,%s\n", result == 0 ? "ok" : "stop");
  return result;
}
