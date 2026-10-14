#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "kdlsym.h"
#include "paging.h"

#define FW_1270 0x12700000U
#define PRESENT (1ULL << 0)
#define RW (1ULL << 1)
#define XOTEXT (1ULL << 58)
#define NX (1ULL << 63)

typedef struct {
  unsigned char prefix[45];
  char message[3075];
} notify_request_t;

extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t,
                                            int);

static void notifyf(const char *fmt, ...)
{
  notify_request_t request = {0};
  va_list args;
  va_start(args, fmt);
  vsnprintf(request.message, sizeof(request.message), fmt, args);
  va_end(args);
  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

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
           "/mnt/usb0/PS5-12.70-text-base-status-%d-%ld.txt",
           (int)getpid(), (long)time(NULL));
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 2;

  char line[256];
  uint64_t pmap = kdlsym(KERNEL_SYM_PMAP_STORE);
  uint64_t pte = 0;
  uint64_t pte_va = find_pte(pmap, text, &pte);
  uint64_t reread = 0;
  snprintf(line, sizeof(line),
           "start,fw=0x%08x,text=0x%016" PRIx64
           ",pte_va=0x%016" PRIx64 ",pte=0x%016" PRIx64 "\n",
           fw, text, pte_va, pte);
  if (save_line(fd, line) != 0) {
    close(fd);
    return 3;
  }
  if (pte_va == UINT64_MAX || read64(pte_va, &reread) != 0 ||
      reread != pte || !(pte & PRESENT) || !(pte & RW) ||
      (pte & (XOTEXT | NX))) {
    save_line(fd, "stop,stage=text-pte-precondition\n");
    close(fd);
    return 4;
  }

  uint64_t cr3 = 0;
  if (save_line(fd, "before,control=pmap-cr3\n") != 0 ||
      read64(pmap + 0x28, &cr3) != 0 || !cr3 || (cr3 & 0xfff)) {
    save_line(fd, "stop,stage=control-read\n");
    close(fd);
    return 5;
  }
  snprintf(line, sizeof(line), "control,cr3=0x%016" PRIx64 "\n", cr3);
  if (save_line(fd, line) != 0 ||
      save_line(fd, "before,text-base-read,bytes=8\n") != 0) {
    close(fd);
    return 6;
  }

  uint64_t value = UINT64_C(0xa5a5a5a5a5a5a5a5);
  int result = kernel_copyout((intptr_t)text, &value, sizeof(value));
  snprintf(line, sizeof(line),
           "after,text-base-read,result=%d,value=0x%016" PRIx64 "\n",
           result, value);
  if (save_line(fd, line) != 0) {
    close(fd);
    return 7;
  }
  if (result != 0 || value == UINT64_MAX ||
      value == UINT64_C(0xa5a5a5a5a5a5a5a5)) {
    save_line(fd, "end,text-unreadable\n");
    close(fd);
    return 8;
  }
  save_line(fd, "end,text-readable\n");
  notifyf("12.70 kernel text first 8 bytes: %016" PRIx64, value);
  return close(fd) == 0 ? 0 : 9;
}
