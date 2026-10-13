#include <inttypes.h>
#include <stdint.h>
#ifdef CARR_SAVE_1270
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef CARR_SAVE_CURRENT_1270
#include <stdio.h>
#include <time.h>
#endif
#endif
#ifdef CARR_LAYOUT_1270
#include <unistd.h>
#endif

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270 0x12700000U
#define CARRIER_PA 0x3ffff000ULL
#define PAGE_BYTES 0x1000U

static uint8_t page[PAGE_BYTES];
#ifdef CARR_LAYOUT_1270
static uint8_t page_after[PAGE_BYTES];
#endif

static uint64_t hash_bytes(const uint8_t *data, unsigned len)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned i = 0; i < len; ++i) {
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
              carrier, hash_bytes(page, PAGE_BYTES), zero, ff, first, last);
#ifdef CARR_SAVE_1270
#ifdef CARR_SAVE_CURRENT_1270
  struct timespec stamp = {0};
  if (clock_gettime(CLOCK_REALTIME, &stamp) != 0)
    return 1;
  char path[128];
  snprintf(path, sizeof(path),
           "/mnt/usb0/PS5-12.70-carrier-3ffff000-%d-%ld.bin",
           (int)getpid(), (long)stamp.tv_sec);
#else
  const char *path = "/mnt/usb0/PS5-12.70-carrier-3ffff000.bin";
#endif
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    klog_printf("CARRSAVE1270,1,stop,stage=open,errno=%d\n", errno);
    return 1;
  }
  unsigned written = 0;
  while (written < PAGE_BYTES) {
    ssize_t n = write(fd, page + written, PAGE_BYTES - written);
    if (n <= 0) {
      klog_printf("CARRSAVE1270,1,stop,stage=write,bytes=%u,errno=%d\n",
                  written, errno);
      close(fd);
      return 1;
    }
    written += (unsigned)n;
  }
  int fsync_rc = fsync(fd);
  int close_rc = close(fd);
  klog_printf("CARRSAVE1270,1,output,path=%s,bytes=%u,fsync=%d,close=%d\n",
              path, written, fsync_rc, close_rc);
  if (fsync_rc != 0 || close_rc != 0)
    return 1;
#endif
#ifdef CARR_LAYOUT_1270
  for (unsigned chunk = 0; chunk < PAGE_BYTES / 0x100; ++chunk) {
    const uint8_t *part = page + chunk * 0x100;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 0x100; ++i)
      nonzero += part[i] != 0;
    uint64_t head = 0;
    for (unsigned i = 0; i < 8; ++i)
      head |= (uint64_t)part[i] << (8 * i);
    klog_printf("CARRLAY1270,1,chunk,offset=0x%03x,nonzero=%u,hash=0x%016" PRIx64
                ",first=0x%016" PRIx64 "\n",
                chunk * 0x100, nonzero, hash_bytes(part, 0x100), head);
  }
  sleep(5);
  if (kernel_copyout((intptr_t)carrier, page_after, sizeof(page_after)) != 0) {
    klog_printf("CARRLAY1270,1,stop,stage=second-read\n");
    return 1;
  }
  unsigned changed = 0;
  for (unsigned i = 0; i < PAGE_BYTES; ++i)
    changed += page[i] != page_after[i];
  klog_printf("CARRLAY1270,1,repeat,seconds=5,changed=%u,hash_before=0x%016" PRIx64
              ",hash_after=0x%016" PRIx64 "\n",
              changed, hash_bytes(page, PAGE_BYTES),
              hash_bytes(page_after, PAGE_BYTES));
#endif
  klog_printf("CARR1270,1,end,ok\n");
  return 0;
}
