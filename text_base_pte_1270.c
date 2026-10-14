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
#define PRESENT       (1ULL << 0)
#define RW            (1ULL << 1)
#define LARGE_PAGE    (1ULL << 7)
#define XOTEXT        (1ULL << 58)
#define NX            (1ULL << 63)
#define PAGE_ADDR     0x000ffffffffff000ULL
#define MAX_TABLE_PA  0x1000000000ULL

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

static int read64(uint64_t va, uint64_t *out)
{
  *out = 0;
  return kernel_copyout((intptr_t)va, out, sizeof(*out));
}

static int table_next(uint64_t dmap, uint64_t entry, uint64_t *out)
{
  uint64_t pa = entry & PAGE_ADDR;
  if (!(entry & PRESENT) || (entry & LARGE_PAGE) || !pa ||
      pa >= MAX_TABLE_PA)
    return -1;
  *out = dmap + pa;
  return 0;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text || data <= text ||
      data - text != 0xd50000ULL)
    return 1;

  char path[160];
  snprintf(path, sizeof(path),
           "/mnt/usb0/PS5-12.70-text-base-pte-%s-%d-%ld.txt",
#ifdef TEXT_PTE_APPLY_1270
           "prepare",
#else
           "inspect",
#endif
           (int)getpid(), (long)time(NULL));
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 2;

  char line[320];
  uint64_t pmap = kdlsym(KERNEL_SYM_PMAP_STORE);
  uint64_t pml4 = 0;
  uint64_t dmap = kdldmap(0);
  snprintf(line, sizeof(line),
           "start,fw=0x%08x,text=0x%016" PRIx64
           ",pmap=0x%016" PRIx64 ",dmap=0x%016" PRIx64 "\n",
           fw, text, pmap, dmap);
  if (save_line(fd, line) != 0 || read64(pmap + 0x20, &pml4) != 0 ||
      pml4 < 0xffff800000000000ULL ||
      dmap < 0xffff800000000000ULL) {
    close(fd);
    return 3;
  }

  const unsigned shifts[4] = {39, 30, 21, 12};
  uint64_t table = pml4, entries[4] = {0}, pte_va = 0;
  for (unsigned i = 0; i < 4; ++i) {
    uint64_t entry_va = table + 8 * ((text >> shifts[i]) & 0x1ffULL);
    if (read64(entry_va, &entries[i]) != 0) {
      snprintf(line, sizeof(line), "stop,stage=read,level=%u\n", i);
      save_line(fd, line);
      close(fd);
      return 4;
    }
    if (i == 3) {
      pte_va = entry_va;
      break;
    }
    if (table_next(dmap, entries[i], &table) != 0) {
      snprintf(line, sizeof(line),
               "stop,stage=table,level=%u,entry=0x%016" PRIx64 "\n",
               i, entries[i]);
      save_line(fd, line);
      close(fd);
      return 5;
    }
  }

  uint64_t old = entries[3];
  uint64_t desired = (old & ~XOTEXT) | RW;
  snprintf(line, sizeof(line),
           "pte,va=0x%016" PRIx64 ",pml4e=0x%016" PRIx64
           ",pdpte=0x%016" PRIx64 ",pde=0x%016" PRIx64
           ",old=0x%016" PRIx64 ",desired=0x%016" PRIx64 "\n",
           pte_va, entries[0], entries[1], entries[2], old, desired);
  if (save_line(fd, line) != 0) {
    close(fd);
    return 6;
  }
  if (!(entries[0] & RW) || !(entries[1] & RW) || !(entries[2] & RW) ||
      (entries[0] & NX) || (entries[1] & NX) || (entries[2] & NX) ||
      !(old & PRESENT) || (old & NX) ||
      !(old & PAGE_ADDR) || (old & PAGE_ADDR) >= MAX_TABLE_PA) {
    save_line(fd, "stop,stage=unexpected-leaf\n");
    close(fd);
    return 7;
  }
  if (desired == old) {
    save_line(fd, "end,already-prepared=1\n");
    return close(fd) == 0 ? 0 : 10;
  }
  if (!(old & XOTEXT)) {
    save_line(fd, "stop,stage=unexpected-flags\n");
    close(fd);
    return 7;
  }

#ifdef TEXT_PTE_APPLY_1270
  if (save_line(fd, "write,before\n") != 0 ||
      kernel_copyin(&desired, (intptr_t)pte_va, sizeof(desired)) != 0) {
    save_line(fd, "stop,stage=write\n");
    close(fd);
    return 8;
  }
  uint64_t observed = 0;
  int read_result = read64(pte_va, &observed);
  snprintf(line, sizeof(line),
           "write,readback=0x%016" PRIx64 ",read_result=%d\n",
           observed, read_result);
  save_line(fd, line);
  if (read_result != 0 || observed != desired) {
    kernel_copyin(&old, (intptr_t)pte_va, sizeof(old));
    save_line(fd, "stop,stage=verify,restore-attempted=1\n");
    close(fd);
    return 9;
  }
  save_line(fd, "end,prepared=1\n");
#else
  save_line(fd, "end,inspect=ok\n");
#endif
  return close(fd) == 0 ? 0 : 10;
}
