#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

/* Verify a readable 4 KiB kernel-data page and its physical direct-map alias. */
#define FW_1270             0x12700000U
#define PAGE_SIZE           0x1000U
#define MAX_PHYSICAL_PA     0x400000000ULL
#define CPU_PRESENT         0x001ULL
#define CPU_LARGE           0x080ULL
#define CPU_FRAME_4K        0x000FFFFFFFFFF000ULL
#define CPU_FRAME_2M        0x000FFFFFFFE00000ULL
#define CPU_FRAME_1G        0x000FFFFFC0000000ULL

typedef struct {
  unsigned char prefix[45];
  char message[3075];
} notify_request_t;
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notifyf(const char *fmt, ...)
{
  notify_request_t req = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static int read_qword(uint64_t kva, uint64_t *out)
{
  *out = 0;
  return kernel_copyout((intptr_t)kva, out, sizeof(*out));
}

static int read_phys(uint64_t dmap, uint64_t pa, uint64_t *out)
{
  if (pa < 0x1000 || pa > MAX_PHYSICAL_PA - sizeof(*out) || (pa & 7))
    return -1;
  return read_qword(dmap + pa, out);
}

static int next_table(uint64_t e, uint64_t *pa)
{
  *pa = e & CPU_FRAME_4K;
  return (e & CPU_PRESENT) && !(e & CPU_LARGE) &&
         *pa >= 0x1000 && *pa < MAX_PHYSICAL_PA ? 0 : -1;
}

static int translate(uint64_t dmap, uint64_t cr3, uint64_t va,
                     uint64_t *out_pa, unsigned *leaf_kb)
{
  uint64_t e = 0, table = cr3;
  if (read_phys(dmap, table + (((va >> 39) & 0x1ff) * 8), &e) != 0 ||
      next_table(e, &table) != 0)
    return -1;
  if (read_phys(dmap, table + (((va >> 30) & 0x1ff) * 8), &e) != 0 ||
      !(e & CPU_PRESENT))
    return -1;
  if (e & CPU_LARGE) {
    *out_pa = (e & CPU_FRAME_1G) | (va & 0x3fffffffULL);
    *leaf_kb = 1048576;
    return 0;
  }
  if (next_table(e, &table) != 0 ||
      read_phys(dmap, table + (((va >> 21) & 0x1ff) * 8), &e) != 0 ||
      !(e & CPU_PRESENT))
    return -1;
  if (e & CPU_LARGE) {
    *out_pa = (e & CPU_FRAME_2M) | (va & 0x1fffffULL);
    *leaf_kb = 2048;
    return 0;
  }
  if (next_table(e, &table) != 0 ||
      read_phys(dmap, table + (((va >> 12) & 0x1ff) * 8), &e) != 0 ||
      !(e & CPU_PRESENT))
    return -1;
  *out_pa = (e & CPU_FRAME_4K) | (va & 0xfffULL);
  *leaf_kb = 4;
  return 0;
}

static uint64_t hash_page(const uint8_t *bytes)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned i = 0; i < PAGE_SIZE; i++) {
    h ^= bytes[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  uint64_t idt = text + g_sym_map_1270[KERNEL_SYM_IDT_ARRAY];
  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data <= text || data - text != 0xD50000ULL ||
      idt != data + 0x2E88300ULL || (data & 0xfff)) {
    klog_printf("GPA1270,1,stop,stage=preflight,fw=0x%08x\n", fw);
    return 1;
  }

  uint64_t pmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t pml4 = 0, cr3 = 0;
  if (read_qword(pmap + 0x20, &pml4) != 0 ||
      read_qword(pmap + 0x28, &cr3) != 0 ||
      !pml4 || !cr3 || pml4 <= cr3 ||
      (pml4 & 0xfff) || (cr3 & 0xfff) || cr3 >= MAX_PHYSICAL_PA) {
    klog_printf("GPA1270,1,stop,stage=pmap\n");
    return 1;
  }
  uint64_t dmap = pml4 - cr3;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL)) {
    klog_printf("GPA1270,1,stop,stage=dmap\n");
    return 1;
  }
  klog_printf("GPA1270,1,start,fw=0x%08x,data=0x%016" PRIx64
              ",cr3=0x%016" PRIx64 ",dmap=0x%016" PRIx64 "\n",
              fw, data, cr3, dmap);

  uint64_t target_pa = 0;
  unsigned leaf_kb = 0;
  if (translate(dmap, cr3, data, &target_pa, &leaf_kb) != 0 ||
      target_pa < 0x1000 || target_pa > MAX_PHYSICAL_PA - PAGE_SIZE ||
      (target_pa & 0xfff) ||
      (target_pa & 0x1fffffULL) > 0x200000ULL - PAGE_SIZE) {
    klog_printf("GPA1270,1,stop,stage=translate,pa=0x%016" PRIx64 "\n",
                target_pa);
    return 1;
  }

  uint8_t kva_bytes[PAGE_SIZE], dmap_bytes[PAGE_SIZE];
  if (kernel_copyout((intptr_t)data, kva_bytes, PAGE_SIZE) != 0 ||
      kernel_copyout((intptr_t)(dmap + target_pa), dmap_bytes, PAGE_SIZE) != 0) {
    klog_printf("GPA1270,1,stop,stage=read,pa=0x%016" PRIx64 "\n", target_pa);
    return 1;
  }
  unsigned match = memcmp(kva_bytes, dmap_bytes, PAGE_SIZE) == 0;
  unsigned nonzero = 0;
  for (unsigned i = 0; i < PAGE_SIZE; i++)
    nonzero += kva_bytes[i] != 0;
  klog_printf("GPA1270,1,target,kva=0x%016" PRIx64 ",pa=0x%016" PRIx64
              ",leaf_kb=%u,within_2m=0x%05" PRIx64
              ",hash=0x%016" PRIx64 ",nonzero=%u,alias_match=%u\n",
              data, target_pa, leaf_kb, target_pa & 0x1fffffULL,
              hash_page(kva_bytes), nonzero, match);
  klog_printf("GPA1270,1,end,%s\n", match && nonzero ? "ok" : "stop");
  notifyf(match && nonzero ? "12.70 kernel data PA verified; see GPA1270 log"
                           : "12.70 kernel data PA check stopped; see GPA1270 log");
  return match && nonzero ? 0 : 1;
}
