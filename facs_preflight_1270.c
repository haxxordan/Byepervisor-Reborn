#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270          0x12700000U
#define TEXT_DATA_GAP    0xD50000ULL
#define FACS_CANDIDATE   0x2CB4C80ULL
#define FADT_DELTA       0x170ULL
#define MAX_PHYSICAL_PA  0x1000000000ULL
#define FADT_SIG         0x50434146U /* "FACP" */
#define DSDT_SIG         0x54445344U /* "DSDT" */

typedef struct {
  unsigned char prefix[45];
  char message[3075];
} notify_request_t;
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify_result(const char *message)
{
  notify_request_t req = {0};
  snprintf(req.message, sizeof(req.message), "%s", message);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static int read_qword(uint64_t addr, uint64_t *out)
{
  *out = 0;
  return kernel_copyout((intptr_t)addr, out, sizeof(*out));
}

static uint32_t get_u32(const uint8_t *bytes, unsigned offset)
{
  uint32_t value;
  memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

static uint64_t get_u64(const uint8_t *bytes, unsigned offset)
{
  uint64_t value;
  memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || !text || data <= text ||
      data - text != TEXT_DATA_GAP ||
      g_sym_map_1270[KERNEL_SYM_PMAP_STORE] - TEXT_DATA_GAP != 0x2E1CFB8ULL) {
    klog_printf("FACS1270,1,stop,stage=preflight,fw=0x%08x\n", fw);
    notify_result("12.70 FACS preflight stopped; see log");
    return 1;
  }

  uint64_t pmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t pml4 = 0, cr3 = 0;
  if (read_qword(pmap + 0x20, &pml4) != 0 ||
      read_qword(pmap + 0x28, &cr3) != 0 ||
      pml4 <= cr3 || (pml4 & 0xfffULL) || (cr3 & 0xfffULL) ||
      cr3 >= MAX_PHYSICAL_PA) {
    klog_printf("FACS1270,1,stop,stage=pmap\n");
    notify_result("12.70 FACS pmap check stopped; see log");
    return 1;
  }
  uint64_t dmap = pml4 - cr3;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL)) {
    klog_printf("FACS1270,1,stop,stage=dmap,value=0x%016" PRIx64 "\n", dmap);
    notify_result("12.70 FACS dmap check stopped; see log");
    return 1;
  }

  uint64_t facs_global = data + FACS_CANDIDATE;
  uint64_t fadt_global = facs_global - FADT_DELTA;
  uint8_t fadt[0xa0] = {0};
  uint64_t facs_ptr = 0, dsdt_ptr = 0;
  uint32_t dsdt_header_sig = 0;
  klog_printf("FACS1270,1,start,fw=0x%08x,data=0x%016" PRIx64
              ",dmap=0x%016" PRIx64 ",candidate=0x%016" PRIx64 "\n",
              fw, data, dmap, facs_global);
  if (kernel_copyout((intptr_t)fadt_global, fadt, sizeof(fadt)) != 0 ||
      read_qword(facs_global, &facs_ptr) != 0 ||
      read_qword(facs_global - 0x30, &dsdt_ptr) != 0 ||
      kernel_copyout((intptr_t)(facs_global - 0x28),
                     &dsdt_header_sig, sizeof(dsdt_header_sig)) != 0) {
    klog_printf("FACS1270,1,stop,stage=data-read\n");
    notify_result("12.70 FACS data read stopped; see log");
    return 1;
  }

  unsigned header_offset = sizeof(fadt);
  for (unsigned i = 0; i <= 0x10; i++) {
    if (get_u32(fadt, i) == FADT_SIG) {
      header_offset = i;
      break;
    }
  }
  uint32_t sig = header_offset < sizeof(fadt) ?
                 get_u32(fadt, header_offset) : get_u32(fadt, 0);
  uint32_t length = header_offset < sizeof(fadt) ?
                    get_u32(fadt, header_offset + 4) : 0;
  uint32_t firmware_ctrl = header_offset < sizeof(fadt) ?
                           get_u32(fadt, header_offset + 0x24) : 0;
  uint32_t dsdt_pa = header_offset < sizeof(fadt) ?
                     get_u32(fadt, header_offset + 0x28) : 0;
  uint64_t x_firmware_ctrl = header_offset < sizeof(fadt) ?
                             get_u64(fadt, header_offset + 0x84) : 0;
  unsigned facs_match = header_offset <= 0x10 && length >= 0x8c &&
                        length <= 0x1000 && firmware_ctrl >= 0x1000 &&
                        facs_ptr == dmap + firmware_ctrl;
  unsigned dsdt_match = dsdt_pa >= 0x1000 &&
                        dsdt_ptr == dmap + dsdt_pa &&
                        dsdt_header_sig == DSDT_SIG;
  klog_printf("FACS1270,1,fadt,window=0x%016" PRIx64
              ",header_offset=%u,sig=0x%08x"
              ",length=%u,firmware_ctrl=0x%08x,x_firmware_ctrl=0x%016" PRIx64
              ",dsdt_pa=0x%08x\n", fadt_global, header_offset, sig, length,
              firmware_ctrl, x_firmware_ctrl, dsdt_pa);
  klog_printf("FACS1270,1,globals,facs_ptr=0x%016" PRIx64
              ",dsdt_ptr=0x%016" PRIx64 ",dsdt_header_sig=0x%08x"
              ",facs_match=%u,dsdt_match=%u\n",
              facs_ptr, dsdt_ptr, dsdt_header_sig,
              facs_match, dsdt_match);
  klog_printf("FACS1270,1,end,ok,candidate_match=%u\n", facs_match);
  notify_result(facs_match ? "12.70 FACS candidate matches FADT + dmap"
                           : "12.70 FACS candidate did not match; see log");
  return 0;
}
