#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

#define FW_1270         0x12700000U
#define PAGE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define MAX_PHYSICAL_PA 0x1000000000ULL
#define PAGE_PRESENT    0x001ULL
#define PAGE_LARGE      0x080ULL
#define CARRIER_PA      0x3FFFF000ULL

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

static int stop_at(const char *stage, uint64_t value)
{
  klog_printf("PREF1270,1,stop,stage=%s,value=0x%016" PRIx64 "\n", stage, value);
  notifyf("12.70 preflight stopped: %s", stage);
  return 1;
}

static int read_qword(uint64_t addr, uint64_t *out)
{
  return kernel_copyout((intptr_t)addr, out, sizeof(*out));
}

static int read_dword(uint64_t addr, uint32_t *out)
{
  return kernel_copyout((intptr_t)addr, out, sizeof(*out));
}

static int next_table(uint64_t dmap, uint64_t entry, uint64_t *table_va)
{
  uint64_t pa = entry & PAGE_ADDR_MASK;
  if (!(entry & PAGE_PRESENT) || !pa || pa >= MAX_PHYSICAL_PA || (pa & 0xfffULL))
    return -1;
  *table_va = dmap + pa;
  return 0;
}

static int inspect_carrier(uint64_t pml4, uint64_t dmap)
{
  uint64_t va = dmap + CARRIER_PA;
  uint64_t pml4e, pdpte, pde, pte, table;
  uint64_t entry_va = pml4 + (((va >> 39) & 0x1ffULL) * 8);
  if (read_qword(entry_va, &pml4e) != 0)
    return stop_at("carrier-pml4e-read", entry_va);
  if (next_table(dmap, pml4e, &table) != 0)
    return stop_at("carrier-pml4e-invalid", pml4e);

  entry_va = table + (((va >> 30) & 0x1ffULL) * 8);
  if (read_qword(entry_va, &pdpte) != 0)
    return stop_at("carrier-pdpte-read", entry_va);
  if (!(pdpte & PAGE_PRESENT))
    return stop_at("carrier-pdpte-not-present", pdpte);
  if (pdpte & PAGE_LARGE) {
    klog_printf("PREF1270,1,carrier,va=0x%016" PRIx64
                ",pml4e=0x%016" PRIx64 ",pdpte=0x%016" PRIx64 ",leaf=1g\n",
                va, pml4e, pdpte);
    return 0;
  }
  if (next_table(dmap, pdpte, &table) != 0)
    return stop_at("carrier-pdpte-invalid", pdpte);

  entry_va = table + (((va >> 21) & 0x1ffULL) * 8);
  if (read_qword(entry_va, &pde) != 0)
    return stop_at("carrier-pde-read", entry_va);
  if (!(pde & PAGE_PRESENT))
    return stop_at("carrier-pde-not-present", pde);
  if (pde & PAGE_LARGE) {
    klog_printf("PREF1270,1,carrier,va=0x%016" PRIx64
                ",pml4e=0x%016" PRIx64 ",pdpte=0x%016" PRIx64
                ",pde=0x%016" PRIx64 ",leaf=2m\n",
                va, pml4e, pdpte, pde);
    return 0;
  }
  if (next_table(dmap, pde, &table) != 0)
    return stop_at("carrier-pde-invalid", pde);

  entry_va = table + (((va >> 12) & 0x1ffULL) * 8);
  if (read_qword(entry_va, &pte) != 0)
    return stop_at("carrier-pte-read", entry_va);
  if (!(pte & PAGE_PRESENT))
    return stop_at("carrier-pte-not-present", pte);
  klog_printf("PREF1270,1,carrier,va=0x%016" PRIx64
              ",pml4e=0x%016" PRIx64 ",pdpte=0x%016" PRIx64
              ",pde=0x%016" PRIx64 ",pte=0x%016" PRIx64 ",leaf=4k\n",
              va, pml4e, pdpte, pde, pte);
  return 0;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270)
    return stop_at("firmware", fw);

  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  if (!text || data <= text || data - text != 0xD50000ULL)
    return stop_at("kernel-bases", data - text);

  uint64_t pmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t pml4, cr3;
  if (read_qword(pmap + 0x20, &pml4) != 0)
    return stop_at("pmap-pml4-read", pmap + 0x20);
  if (read_qword(pmap + 0x28, &cr3) != 0)
    return stop_at("pmap-cr3-read", pmap + 0x28);
  if (pml4 <= cr3 || (pml4 & 0xfffULL) || (cr3 & 0xfffULL) ||
      cr3 >= MAX_PHYSICAL_PA)
    return stop_at("pmap-fields", pml4);

  uint64_t dmap = pml4 - cr3;
  if ((dmap >> 48) != 0xffffULL || (dmap & 0x3fffffffULL))
    return stop_at("dmap-base", dmap);
  klog_printf("PREF1270,1,pmap,pml4=0x%016" PRIx64 ",cr3=0x%016" PRIx64
              ",dmap=0x%016" PRIx64 "\n", pml4, cr3, dmap);

  uint32_t dmpml4i, dmpdpi;
  if (read_dword(text + g_sym_map_1270[KERNEL_SYM_DMPML4I], &dmpml4i) != 0)
    return stop_at("dmpml4i-read", text + g_sym_map_1270[KERNEL_SYM_DMPML4I]);
  if (read_dword(text + g_sym_map_1270[KERNEL_SYM_DMPDPI], &dmpdpi) != 0)
    return stop_at("dmpdpi-read", text + g_sym_map_1270[KERNEL_SYM_DMPDPI]);
  uint64_t dmap_globals = 0xFFFF800000000000ULL | ((uint64_t)dmpml4i << 39)
                         | ((uint64_t)dmpdpi << 30);
  klog_printf("PREF1270,1,dmap_globals,pml4i=%u,pdpi=%u,base=0x%016" PRIx64
              ",match=%u\n", dmpml4i, dmpdpi, dmap_globals,
              (unsigned)(dmap_globals == dmap));

  if (inspect_carrier(pml4, dmap) != 0)
    return 1;

  uint64_t cave = text + g_sym_map_1270[KERNEL_SYM_DATA_CAVE];
  uint64_t oracle, state, scratch;
  if (read_qword(cave + 0x440, &oracle) != 0)
    return stop_at("cave-oracle-read", cave + 0x440);
  if (read_qword(cave + 0x600, &state) != 0)
    return stop_at("cave-state-read", cave + 0x600);
  if (read_qword(cave + 0x3a00, &scratch) != 0)
    return stop_at("cave-scratch-read", cave + 0x3a00);
  klog_printf("PREF1270,1,cave,base=0x%016" PRIx64
              ",oracle=0x%016" PRIx64 ",state=0x%016" PRIx64
              ",scratch=0x%016" PRIx64 "\n", cave, oracle, state, scratch);

  uint64_t idt = text + g_sym_map_1270[KERNEL_SYM_IDT_ARRAY];
  uint64_t gate_lo, gate_hi;
  if (read_qword(idt + 0x10, &gate_lo) != 0)
    return stop_at("db-gate-lo-read", idt + 0x10);
  if (read_qword(idt + 0x18, &gate_hi) != 0)
    return stop_at("db-gate-hi-read", idt + 0x18);
  uint64_t handler = (gate_lo & 0xffffULL)
                   | (((gate_lo >> 48) & 0xffffULL) << 16)
                   | ((gate_hi & 0xffffffffULL) << 32);
  unsigned present = (unsigned)((gate_lo >> 47) & 1);
  unsigned selector = (unsigned)((gate_lo >> 16) & 0xffff);
  unsigned type = (unsigned)((gate_lo >> 40) & 0xf);
  klog_printf("PREF1270,1,db_gate,lo=0x%016" PRIx64 ",hi=0x%016" PRIx64
              ",handler=0x%016" PRIx64 ",present=%u,selector=0x%x,type=0x%x"
              ",in_kimage=%u\n", gate_lo, gate_hi, handler,
              present, selector, type,
              (unsigned)(handler >= text && handler - text < 0x4000000ULL));

  uint64_t tss = text + g_sym_map_1270[KERNEL_SYM_COMMON_TSS];
  uint64_t rsp0, ist6;
  if (read_qword(tss + 0x04, &rsp0) != 0)
    return stop_at("tss-rsp0-read", tss + 0x04);
  if (read_qword(tss + 0x4c, &ist6) != 0)
    return stop_at("tss-ist6-read", tss + 0x4c);
  klog_printf("PREF1270,1,tss,base=0x%016" PRIx64
              ",rsp0=0x%016" PRIx64 ",ist6=0x%016" PRIx64
              ",rsp0_canonical=%u\n", tss, rsp0, ist6,
              (unsigned)((rsp0 >> 47) == 0x1ffffULL));

  klog_printf("PREF1270,1,end,ok\n");
  notifyf("12.70 preflight complete; see PREF1270 log");
  return 0;
}
