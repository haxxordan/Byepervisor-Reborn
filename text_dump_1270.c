#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define FW_1270       0x12700000U
#define TEXT_BYTES    0xd50000ULL
#define PAGE_BYTES    0x1000U
#define SYNC_BYTES    0x10000U

static unsigned char page[PAGE_BYTES];

static int write_all(int fd, const void *buf, size_t bytes)
{
  const unsigned char *p = buf;
  while (bytes) {
    ssize_t n = write(fd, p, bytes);
    if (n <= 0)
      return -1;
    p += n;
    bytes -= (size_t)n;
  }
  return 0;
}

static int save_line(int fd, const char *line)
{
  return write_all(fd, line, strlen(line)) == 0 ? fsync(fd) : -1;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text || data <= text ||
      data - text != TEXT_BYTES)
    return 1;

  char bin_path[192], log_path[192], line[256];
  int pid = (int)getpid();
  long stamp = (long)time(NULL);
  snprintf(bin_path, sizeof(bin_path),
           "/mnt/usb0/PS5-12.70-ktext-%d-%ld.bin", pid, stamp);
  snprintf(log_path, sizeof(log_path),
           "/mnt/usb0/PS5-12.70-ktext-%d-%ld.txt", pid, stamp);
  int log_fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (log_fd < 0)
    return 2;
  int bin_fd = open(bin_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (bin_fd < 0) {
    save_line(log_fd, "stop,stage=bin-open\n");
    close(log_fd);
    return 3;
  }

  snprintf(line, sizeof(line),
           "start,fw=0x%08x,text=0x%016" PRIx64 ",bytes=%" PRIu64 "\n",
           fw, text, (uint64_t)TEXT_BYTES);
  if (save_line(log_fd, line) != 0) {
    close(bin_fd);
    close(log_fd);
    return 4;
  }

  for (uint64_t off = 0; off < TEXT_BYTES; off += PAGE_BYTES) {
    if (off < SYNC_BYTES || (off % SYNC_BYTES) == 0) {
      snprintf(line, sizeof(line), "before,offset=0x%08" PRIx64 "\n", off);
      if (save_line(log_fd, line) != 0) {
        close(bin_fd);
        close(log_fd);
        return 5;
      }
    }

    int result = kernel_copyout((intptr_t)(text + off), page, PAGE_BYTES);
    if (result != 0) {
      snprintf(line, sizeof(line),
               "stop,stage=copyout,offset=0x%08" PRIx64 ",result=%d\n",
               off, result);
      save_line(log_fd, line);
      fsync(bin_fd);
      close(bin_fd);
      close(log_fd);
      return 6;
    }
    if (off == 0) {
      uint64_t first = 0;
      memcpy(&first, page, sizeof(first));
      snprintf(line, sizeof(line), "first8,value=0x%016" PRIx64 "\n", first);
      if (save_line(log_fd, line) != 0 ||
          first != UINT64_C(0x0000000058c55be8)) {
        save_line(log_fd, "stop,stage=first8-mismatch\n");
        close(bin_fd);
        close(log_fd);
        return 7;
      }
    }
    if (write_all(bin_fd, page, PAGE_BYTES) != 0) {
      snprintf(line, sizeof(line),
               "stop,stage=write,offset=0x%08" PRIx64 ",errno=%d\n",
               off, errno);
      save_line(log_fd, line);
      close(bin_fd);
      close(log_fd);
      return 8;
    }
    if (off < SYNC_BYTES || ((off + PAGE_BYTES) % SYNC_BYTES) == 0 ||
        off + PAGE_BYTES == TEXT_BYTES) {
      int synced = fsync(bin_fd);
      snprintf(line, sizeof(line),
               "committed,bytes=0x%08" PRIx64 ",fsync=%d\n",
               off + PAGE_BYTES, synced);
      if (synced != 0 || save_line(log_fd, line) != 0) {
        close(bin_fd);
        close(log_fd);
        return 9;
      }
    }
  }

  if (save_line(log_fd, "end,complete=1\n") != 0) {
    close(bin_fd);
    close(log_fd);
    return 10;
  }
  int bin_close = close(bin_fd);
  int log_close = close(log_fd);
  return bin_close == 0 && log_close == 0 ? 0 : 11;
}
