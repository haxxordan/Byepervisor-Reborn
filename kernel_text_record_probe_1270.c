#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define FW_1270 0x12700000U
#define PMAP_STORE_OFFSET UINT64_C(0x3b6cfb8)

static int save_line(int fd, const char *line)
{
  size_t len = strlen(line);
  size_t done = 0;
  while (done < len) {
    ssize_t n = write(fd, line + done, len - done);
    if (n <= 0)
      return -1;
    done += (size_t)n;
  }
  return fsync(fd);
}

static int read_qword(int fd, const char *name, uint64_t va)
{
  char line[192];
  uint64_t value = UINT64_C(0xa5a5a5a5a5a5a5a5);
  snprintf(line, sizeof(line), "before,%s,va=0x%016" PRIx64 "\n", name, va);
  if (save_line(fd, line) != 0)
    return -2;

  int result = kernel_copyout((intptr_t)va, &value, sizeof(value));
  snprintf(line, sizeof(line),
           "after,%s,result=%d,value=0x%016" PRIx64 "\n",
           name, result, value);
  if (save_line(fd, line) != 0)
    return -3;
  return result;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data - text != UINT64_C(0xd50000))
    return 1;

  char path[128];
  snprintf(path, sizeof(path),
           "/mnt/usb0/PS5-12.70-ktextread-%d.txt", (int)getpid());
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 2;

  char line[192];
  snprintf(line, sizeof(line),
           "start,fw=0x%08x,text=0x%016" PRIx64
           ",data=0x%016" PRIx64 "\n", fw, text, data);
  if (save_line(fd, line) != 0) {
    close(fd);
    return 3;
  }

  int result = read_qword(fd, "pmap-cr3", text + PMAP_STORE_OFFSET + 0x28);
  if (result != 0) {
    close(fd);
    return 4;
  }
  result = read_qword(fd, "text-base", text);
  if (result != 0) {
    close(fd);
    return 5;
  }
  if (save_line(fd, "end,ok\n") != 0) {
    close(fd);
    return 6;
  }
  return close(fd) == 0 ? 0 : 7;
}
