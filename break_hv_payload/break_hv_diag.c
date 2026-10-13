#include <stdint.h>

/* The AP diagnostic uses the same entry trampoline as the full payload. It
 * reads two candidate HV globals and writes only to the guest-side oracle. */
#define AP_DIAG_ENTERED 0x415031323730454eULL
#define AP_DIAG_DONE    0x415031323730444eULL
#define AP_DIAG_BAD_PA  0x4150313237304241ULL
#define MAX_PHYS_PA     0x1000000000ULL
#define EXPECTED_APS    15ULL

static uint64_t read64(uint64_t addr)
{
  return *(volatile uint64_t *)(uintptr_t)addr;
}

static void write64(uint64_t addr, uint64_t value)
{
  *(volatile uint64_t *)(uintptr_t)addr = value;
}

void vmcb_disable_np(uint64_t *param)
{
  const uint64_t dmap = param[1];
  const uint64_t oracle = param[2];
  const uint64_t heap_pa = param[4];
  const uint64_t pcpus_pa = param[5];

  write64(oracle, AP_DIAG_ENTERED);
  if (heap_pa >= MAX_PHYS_PA || pcpus_pa >= MAX_PHYS_PA ||
      heap_pa + 8 > MAX_PHYS_PA || pcpus_pa + 8 > MAX_PHYS_PA) {
    write64(oracle, AP_DIAG_BAD_PA);
    return;
  }

  write64(oracle + 8, read64(dmap + heap_pa));
  write64(oracle + 16, read64(dmap + pcpus_pa));
  uint64_t finished = __atomic_add_fetch((uint64_t *)(uintptr_t)(oracle + 24),
                                         1, __ATOMIC_SEQ_CST);
  if (finished >= EXPECTED_APS)
    write64(oracle, AP_DIAG_DONE);
}
