#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270             0x12700000U
#define IDT_TO_GVMSPACE     0x9D90ULL
#define DATA_GVMSPACE_OFF   0x2E7E570ULL
#define GVM_ENTRY_SIZE      0x100ULL
#define GVM_ENTRY_READ_SIZE 0x40U
#define GVM_ENTRY_COUNT     16U

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

static uint64_t qword_at(const unsigned char *bytes, unsigned off)
{
  uint64_t value = 0;
  memcpy(&value, bytes + off, sizeof(value));
  return value;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog_printf("GVM1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 1;
  }

  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if (!text || data <= text || data - text != 0xD50000ULL) {
    klog_printf("GVM1270,1,stop,stage=bases,text=0x%016" PRIx64
                ",data=0x%016" PRIx64 "\n", text, data);
    return 1;
  }

  uint64_t idt = text + g_sym_map_1270[KERNEL_SYM_IDT_ARRAY];
  if (idt != data + 0x2E88300ULL || idt < IDT_TO_GVMSPACE) {
    klog_printf("GVM1270,1,stop,stage=idt,addr=0x%016" PRIx64 "\n", idt);
    return 1;
  }
  uint64_t gvmspace = idt - IDT_TO_GVMSPACE;
  if (gvmspace != data + DATA_GVMSPACE_OFF) {
    klog_printf("GVM1270,1,stop,stage=offset,addr=0x%016" PRIx64 "\n", gvmspace);
    return 1;
  }

  uint64_t gate[2] = {0};
  if (kernel_copyout((intptr_t)(idt + 0x10), gate, sizeof(gate)) != 0 ||
      ((gate[0] >> 47) & 1) != 1 ||
      ((gate[0] >> 40) & 0xf) != 0xe ||
      ((gate[0] >> 16) & 0xffff) != 0x20) {
    klog_printf("GVM1270,1,stop,stage=gate\n");
    return 1;
  }

  klog_printf("GVM1270,1,start,fw=0x%08x,data=0x%016" PRIx64
              ",idt=0x%016" PRIx64 ",gvmspace=0x%016" PRIx64 "\n",
              fw, data, idt, gvmspace);

  unsigned nonfree = 0, plausible_dirs = 0;
  for (unsigned i = 0; i < GVM_ENTRY_COUNT; i++) {
    unsigned char row[GVM_ENTRY_READ_SIZE] = {0};
    uint64_t entry = gvmspace + i * GVM_ENTRY_SIZE;
    if (kernel_copyout((intptr_t)entry, row, sizeof(row)) != 0) {
      klog_printf("GVM1270,1,stop,stage=row-read,index=%u,addr=0x%016" PRIx64 "\n",
                  i, entry);
      notifyf("12.70 GPU VM inventory stopped at entry %u", i);
      return 1;
    }
    uint64_t state = qword_at(row, 0x00);
    uint64_t start_va = qword_at(row, 0x08);
    uint64_t size = qword_at(row, 0x10);
    uint64_t active = qword_at(row, 0x18);
    uint64_t pdb2_pa = qword_at(row, 0x28);
    uint64_t page_dir_va = qword_at(row, 0x38);
    if ((uint32_t)state != 0x20U)
      nonfree++;
    unsigned plausible = (unsigned)(page_dir_va != 0 &&
                                     (page_dir_va >> 48) == 0xffffULL &&
                                     (page_dir_va & 0xfffULL) == 0);
    plausible_dirs += plausible;
    klog_printf("GVM1270,1,row,index=%u,state=0x%016" PRIx64
                ",start=0x%016" PRIx64 ",size=0x%016" PRIx64
                ",active=0x%016" PRIx64 ",pdb2_pa=0x%016" PRIx64
                ",page_dir=0x%016" PRIx64 ",dir_plausible=%u\n",
                i, state, start_va, size, active, pdb2_pa, page_dir_va,
                plausible);
  }
  klog_printf("GVM1270,1,end,ok,nonfree=%u,plausible_dirs=%u,total=%u\n",
              nonfree, plausible_dirs, GVM_ENTRY_COUNT);
  notifyf("12.70 GPU VM inventory complete; see GVM1270 log");
  return 0;
}
