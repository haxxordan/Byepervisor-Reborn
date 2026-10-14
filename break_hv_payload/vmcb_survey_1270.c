#include <stdint.h>

#define ENTERED 0x1270a9e17e5e1001ULL
#define COMPLETE 0x1270a9e17e5e1002ULL
#define REJECTED 0x1270a9e17e5e1003ULL
#define VMCB_BASE_PA 0x62a57000ULL
#define VMCB_STRIDE 0x2000ULL

static uint64_t read64(uint64_t va)
{
  return *(volatile uint64_t *)(uintptr_t)va;
}

static void write64(uint64_t va, uint64_t value)
{
  *(volatile uint64_t *)(uintptr_t)va = value;
}

void vmcb_probe_1270(uint64_t *param)
{
  uint64_t dmap = param[1];
  uint64_t oracle = param[2];
  write64(oracle, ENTERED);
  for (uint64_t core = 0; core < 16; ++core) {
    uint64_t vpa = VMCB_BASE_PA + core * VMCB_STRIDE;
    uint64_t ctrl = read64(dmap + vpa + 0x90);
    uint64_t ncr3 = read64(dmap + vpa + 0xb0);
    if (ctrl != 9 || ncr3 < 0x62000000ULL ||
        ncr3 >= 0x63000000ULL || (ncr3 & 0xfff)) {
      write64(oracle + 8, core);
      write64(oracle, REJECTED);
      return;
    }
  }
  write64(oracle + 8, 16);
  write64(oracle, COMPLETE);
}
