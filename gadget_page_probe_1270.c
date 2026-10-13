#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270          0x12700000U
#define PAGE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define MAX_PHYSICAL_PA  0x1000000000ULL
#define PAGE_PRESENT     (1ULL << 0)
#define PAGE_LARGE       (1ULL << 7)
#define PAGE_XOTEXT      (1ULL << 58)
#define PAGE_NX          (1ULL << 63)

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

static int read_qword(uint64_t addr, uint64_t *out)
{
  *out = 0;
  return kernel_copyout((intptr_t)addr, out, sizeof(*out));
}

static int next_table(uint64_t dmap, uint64_t entry, uint64_t *table_va)
{
  uint64_t pa = entry & PAGE_ADDR_MASK;
  if (!(entry & PAGE_PRESENT) || (entry & PAGE_LARGE) || !pa ||
      pa >= MAX_PHYSICAL_PA || (pa & 0xfffULL))
    return -1;
  *table_va = dmap + pa;
  return 0;
}

static int inspect_va(uint64_t pml4, uint64_t dmap, uint64_t va,
                      const char *name, unsigned *nx_count)
{
  uint64_t e = 0, table = pml4, leaf = 0;
  uint64_t levels[4] = {0};
  unsigned leaf_size = 0;
  unsigned nx = 0;
  const unsigned shifts[4] = {39, 30, 21, 12};

  for (unsigned i = 0; i < 4; i++) {
    uint64_t entry_va = table + (((va >> shifts[i]) & 0x1ffULL) * 8);
    if (read_qword(entry_va, &e) != 0) {
      klog_printf("GPG1270,1,stop,name=%s,stage=read,level=%u,addr=0x%016" PRIx64 "\n",
                  name, i, entry_va);
      return -1;
    }
    levels[i] = e;
    nx |= (unsigned)((e & PAGE_NX) != 0);
    if (!(e & PAGE_PRESENT)) {
      klog_printf("GPG1270,1,stop,name=%s,stage=absent,level=%u,entry=0x%016" PRIx64 "\n",
                  name, i, e);
      return -1;
    }
    if ((i == 1 || i == 2) && (e & PAGE_LARGE)) {
      leaf = e;
      leaf_size = (i == 1) ? 1024 : 2;
      break;
    }
    if (i == 3) {
      leaf = e;
      leaf_size = 0;
      break;
    }
    if (next_table(dmap, e, &table) != 0) {
      klog_printf("GPG1270,1,stop,name=%s,stage=table,level=%u,entry=0x%016" PRIx64 "\n",
                  name, i, e);
      return -1;
    }
  }

  *nx_count += nx;
  klog_printf("GPG1270,1,page,name=%s,va=0x%016" PRIx64
              ",above_data=%u,leaf_mb=%u,nx=%u,xotext=%u,pml4e=0x%016" PRIx64
              ",pdpte=0x%016" PRIx64 ",pde=0x%016" PRIx64
              ",leaf=0x%016" PRIx64 "\n",
              name, va, (unsigned)(va >= (uint64_t)KERNEL_ADDRESS_DATA_BASE),
              leaf_size, nx, (unsigned)((leaf & PAGE_XOTEXT) != 0),
              levels[0], levels[1], levels[2], leaf);
  return 0;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog_printf("GPG1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 1;
  }

  const uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  const uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if (!text || data <= text || data - text != 0xD50000ULL) {
    klog_printf("GPG1270,1,stop,stage=bases,text=0x%016" PRIx64
                ",data=0x%016" PRIx64 "\n", text, data);
    return 1;
  }

  uint64_t pmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t pml4 = 0, cr3 = 0;
  if (read_qword(pmap + 0x20, &pml4) != 0 ||
      read_qword(pmap + 0x28, &cr3) != 0 ||
      !pml4 || !cr3 || (pml4 & 0xfffULL) || (cr3 & 0xfffULL) ||
      pml4 <= cr3 || cr3 >= MAX_PHYSICAL_PA) {
    klog_printf("GPG1270,1,stop,stage=pmap\n");
    return 1;
  }
  uint64_t dmap = pml4 - cr3;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL)) {
    klog_printf("GPG1270,1,stop,stage=dmap,base=0x%016" PRIx64 "\n", dmap);
    return 1;
  }

  const struct {
    const char *name;
    uint64_t offset;
  } targets[] = {
    {"pivot", g_gadget_map_1270[KERNEL_GADGET_PIVOT_ADD_RSP28_RET]},
    {"iretq", g_gadget_map_1270[KERNEL_GADGET_IRETQ]},
    {"pop_rax", g_gadget_map_1270[KERNEL_GADGET_POP_RAX]},
    {"pop_rcx", g_gadget_map_1270[KERNEL_GADGET_POP_RCX]},
    {"pop_rdi", g_gadget_map_1270[KERNEL_GADGET_POP_RDI]},
    {"pop_rsi", g_gadget_map_1270[KERNEL_GADGET_POP_RSI]},
    {"pop_rdx", g_gadget_map_1270[KERNEL_GADGET_POP_RDX]},
    {"pop_r8", g_gadget_map_1270[KERNEL_GADGET_POP_R8]},
    {"pop_rsp", g_gadget_map_1270[KERNEL_GADGET_POP_RSP]},
    {"store_rcx_r8", g_gadget_map_1270[KERNEL_GADGET_STORE_RCX_R8]},
    {"store_rdi8_rcx", g_gadget_map_1270[KERNEL_GADGET_STORE_RDI8_RCX]},
    {"or_rcx_eax", g_gadget_map_1270[KERNEL_GADGET_OR_RCX_EAX]},
    {"set_dr7", g_gadget_map_1270[KERNEL_GADGET_SET_DR7]},
    {"st_rcx_rdx", g_gadget_map_1270[KERNEL_GADGET_ST_RCX_RDX]},
    {"st_rcx_rax", g_gadget_map_1270[KERNEL_GADGET_ST_RCX_RAX]},
    {"mov_rax_rsi", g_gadget_map_1270[KERNEL_GADGET_MOV_RAX_RSI]},
    {"mov_rax_rdi", g_gadget_map_1270[KERNEL_GADGET_MOV_RAX_RDI]},
    {"rep_movsb", g_gadget_map_1270[KERNEL_GADGET_REP_MOVSB]},
    {"cld", g_gadget_map_1270[KERNEL_GADGET_CLD]},
    {"ret_tramp", g_gadget_map_1270[KERNEL_GADGET_RET_TRAMP]},
    {"dbreg_resume", g_gadget_map_1270[KERNEL_GADGET_DBREG_RESUME]},
    {"startup_call", g_sym_map_1270[KERNEL_SYM_HV_MP_STARTUP_CALL]},
    {"mp_init_vector", g_sym_map_1270[KERNEL_SYM_MP_STARTUP_INIT_VECTOR]},
    {"sleep_hook", g_sym_map_1270[KERNEL_SYM_ACPI_SLEEP_MACHDEP_HOOK]},
    // Public kstuff-lite 12_70.h offsets converted from kdata-relative using
    // the observed 0xD50000 text/data gap. Mapping checks do not verify bytes.
    {"public_iretq", 0x2B34ADULL},
    {"public_rep_movsb", 0x2EFC3AULL},
    {"public_store_rax_rdi", 0x2ABF92ULL},
  };
  unsigned nx_count = 0;
  klog_printf("GPG1270,1,start,fw=0x%08x,text=0x%016" PRIx64
              ",data=0x%016" PRIx64 ",dmap=0x%016" PRIx64 "\n",
              fw, text, data, dmap);
  for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    if (inspect_va(pml4, dmap, text + targets[i].offset,
                   targets[i].name, &nx_count) != 0) {
      notifyf("12.70 gadget page probe stopped: %s", targets[i].name);
      return 1;
    }
  }
  klog_printf("GPG1270,1,end,ok,nx_targets=%u,total=%u\n", nx_count,
              (unsigned)(sizeof(targets) / sizeof(targets[0])));
  notifyf("12.70 gadget pages checked: %u NX; see GPG1270 log", nx_count);
  return 0;
}
