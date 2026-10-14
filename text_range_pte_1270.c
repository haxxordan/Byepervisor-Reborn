#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "kdlsym.h"

#define FW_1270       0x12700000U
#define TEXT_BYTES    0xd50000ULL
#define PAGE_BYTES    0x1000ULL
#define LARGE_BYTES   0x200000ULL
#define PRESENT       (1ULL << 0)
#define RW            (1ULL << 1)
#define LARGE_PAGE    (1ULL << 7)
#define XOTEXT        (1ULL << 58)
#define NX            (1ULL << 63)
#define A_D_BITS      ((1ULL << 5) | (1ULL << 6))
#define FRAME_4K      0x000ffffffffff000ULL
#define FRAME_2M      0x000fffffffe00000ULL
#define RAM_LIMIT     0x1000000000ULL
#define MAX_CHANGES   (TEXT_BYTES / PAGE_BYTES + 16)

typedef struct {
  uint64_t entry_va;
  uint64_t old;
  uint64_t desired;
} change_t;

static change_t changes[MAX_CHANGES];
static unsigned change_count;

static int save_line(int fd, const char *line)
{
  size_t size = strlen(line);
  while (size) {
    ssize_t wrote = write(fd, line, size);
    if (wrote <= 0)
      return -1;
    line += wrote;
    size -= (size_t)wrote;
  }
  return fsync(fd);
}

static int read64(uint64_t va, uint64_t *value)
{
  *value = 0;
  return kernel_copyout((intptr_t)va, value, sizeof(*value));
}

static int table_next(uint64_t dmap, uint64_t entry, uint64_t *table)
{
  uint64_t pa = entry & FRAME_4K;
  if (!(entry & PRESENT) || (entry & LARGE_PAGE) || !pa ||
      pa >= RAM_LIMIT)
    return -1;
  *table = dmap + pa;
  return 0;
}

static int add_change(uint64_t entry_va, uint64_t old)
{
  uint64_t desired = (old & ~XOTEXT) | RW;
  if (desired == old)
    return 0;
  if (change_count >= MAX_CHANGES)
    return -1;
  changes[change_count++] = (change_t){entry_va, old, desired};
  return 0;
}

#ifdef TEXT_RANGE_APPLY_1270
static int restore_change(const change_t *change)
{
  uint64_t current = 0, observed = 0;
  if (read64(change->entry_va, &current) != 0)
    return -1;
  uint64_t restored = (current & ~(XOTEXT | RW)) |
                      (change->old & (XOTEXT | RW));
  return kernel_copyin(&restored, (intptr_t)change->entry_va, 8) == 0 &&
         read64(change->entry_va, &observed) == 0 &&
         ((observed ^ restored) & ~A_D_BITS) == 0 ? 0 : -1;
}
#endif

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text || data <= text ||
      data - text != TEXT_BYTES)
    return 1;

  char path[192], line[256];
  snprintf(path, sizeof(path),
           "/mnt/usb0/PS5-12.70-text-range-%s-%d-%ld.txt",
#ifdef TEXT_RANGE_APPLY_1270
           "apply",
#else
           "plan",
#endif
           (int)getpid(), (long)time(NULL));
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 2;

  uint64_t pmap = kdlsym(KERNEL_SYM_PMAP_STORE);
  uint64_t dmap = kdldmap(0);
  uint64_t pml4 = 0;
  snprintf(line, sizeof(line),
           "start,fw=0x%08x,text=0x%016" PRIx64
           ",data=0x%016" PRIx64 ",pmap=0x%016" PRIx64 "\n",
           fw, text, data, pmap);
  if (save_line(fd, line) != 0 || read64(pmap + 0x20, &pml4) != 0 ||
      pml4 < 0xffff800000000000ULL || dmap < 0xffff800000000000ULL) {
    save_line(fd, "stop,stage=root\n");
    close(fd);
    return 3;
  }

  unsigned pages_4k = 0, leaves_2m = 0, already = 0;
  uint64_t last_pde_va = UINT64_MAX;
  for (uint64_t off = 0; off < TEXT_BYTES;) {
    uint64_t va = text + off, table = pml4, entry = 0;
    unsigned shifts[3] = {39, 30, 21};
    uint64_t entry_va = 0;
    for (unsigned level = 0; level < 3; ++level) {
      entry_va = table + 8 * ((va >> shifts[level]) & 0x1ffULL);
      if (read64(entry_va, &entry) != 0 || !(entry & PRESENT) ||
          (entry & NX)) {
        snprintf(line, sizeof(line),
                 "stop,stage=walk,offset=0x%08" PRIx64 ",level=%u\n",
                 off, level);
        save_line(fd, line);
        close(fd);
        return 4;
      }
      if (level < 2) {
        if (!(entry & RW) || (entry & XOTEXT) ||
            table_next(dmap, entry, &table) != 0) {
          snprintf(line, sizeof(line),
                   "stop,stage=parent,offset=0x%08" PRIx64 ",level=%u"
                   ",entry=0x%016" PRIx64 "\n", off, level, entry);
          save_line(fd, line);
          close(fd);
          return 5;
        }
      }
    }

    if (entry & LARGE_PAGE) {
      uint64_t pa = entry & FRAME_2M;
      if (!pa || pa >= RAM_LIMIT || add_change(entry_va, entry) != 0) {
        save_line(fd, "stop,stage=large-leaf\n");
        close(fd);
        return 6;
      }
      if (((entry & ~XOTEXT) | RW) == entry)
        ++already;
      ++leaves_2m;
      uint64_t advance = LARGE_BYTES - (va & (LARGE_BYTES - 1));
      off += advance < TEXT_BYTES - off ? advance : TEXT_BYTES - off;
      continue;
    }

    if (entry_va != last_pde_va) {
      if (table_next(dmap, entry, &table) != 0 ||
          add_change(entry_va, entry) != 0) {
        save_line(fd, "stop,stage=page-table\n");
        close(fd);
        return 7;
      }
      last_pde_va = entry_va;
    } else if (table_next(dmap, entry, &table) != 0) {
      save_line(fd, "stop,stage=page-table-repeat\n");
      close(fd);
      return 7;
    }
    uint64_t pte_va = table + 8 * ((va >> 12) & 0x1ffULL);
    uint64_t pte = 0;
    if (read64(pte_va, &pte) != 0 || !(pte & PRESENT) || (pte & NX) ||
        !(pte & FRAME_4K) || (pte & FRAME_4K) >= RAM_LIMIT ||
        add_change(pte_va, pte) != 0) {
      snprintf(line, sizeof(line),
               "stop,stage=small-leaf,offset=0x%08" PRIx64 "\n", off);
      save_line(fd, line);
      close(fd);
      return 8;
    }
    if (((pte & ~XOTEXT) | RW) == pte)
      ++already;
    ++pages_4k;
    off += PAGE_BYTES;
  }

  snprintf(line, sizeof(line),
           "plan,changes=%u,pages_4k=%u,leaves_2m=%u,already=%u\n",
           change_count, pages_4k, leaves_2m, already);
  if (save_line(fd, line) != 0) {
    close(fd);
    return 9;
  }
  for (unsigned i = 0; i < change_count; ++i) {
    snprintf(line, sizeof(line),
             "entry,index=%u,va=0x%016" PRIx64
             ",old=0x%016" PRIx64 ",desired=0x%016" PRIx64 "\n",
             i, changes[i].entry_va, changes[i].old, changes[i].desired);
    if (save_line(fd, line) != 0) {
      close(fd);
      return 10;
    }
  }
  if (save_line(fd, "plan,saved=1\n") != 0) {
    close(fd);
    return 10;
  }

#ifdef TEXT_RANGE_APPLY_1270
  unsigned applied = 0;
  for (unsigned i = 0; i < change_count; ++i) {
    uint64_t current = 0, observed = 0, desired = 0;
    int attempted = 0;
    if (read64(changes[i].entry_va, &current) != 0 ||
        ((current ^ changes[i].old) & ~A_D_BITS) != 0)
      goto apply_failed;
    changes[i].old = current;
    desired = (current & ~XOTEXT) | RW;
    attempted = 1;
    if (kernel_copyin(&desired,
                      (intptr_t)changes[i].entry_va, 8) != 0 ||
        read64(changes[i].entry_va, &observed) != 0 ||
        ((observed ^ desired) & ~A_D_BITS) != 0) {
apply_failed:
      snprintf(line, sizeof(line),
               "stop,stage=apply,index=%u,applied=%u,current=0x%016" PRIx64
               ",observed=0x%016" PRIx64 "\n",
               i, applied, current, observed);
      save_line(fd, line);
      unsigned rollback_failed = 0;
      if (attempted && restore_change(&changes[i]) != 0)
        ++rollback_failed;
      while (applied) {
        --applied;
        if (restore_change(&changes[applied]) != 0)
          ++rollback_failed;
      }
      snprintf(line, sizeof(line),
               "rollback,attempted=1,failed=%u\n", rollback_failed);
      save_line(fd, line);
      close(fd);
      return 11;
    }
    ++applied;
    if ((applied % 32) == 0) {
      snprintf(line, sizeof(line), "apply,verified=%u\n", applied);
      if (save_line(fd, line) != 0) {
        close(fd);
        return 12;
      }
    }
  }
  snprintf(line, sizeof(line), "end,applied=%u,verified=1\n", applied);
#else
  snprintf(line, sizeof(line), "end,plan-only=1\n");
#endif
  int saved = save_line(fd, line);
  int closed = close(fd);
  return saved == 0 && closed == 0 ? 0 : 13;
}
