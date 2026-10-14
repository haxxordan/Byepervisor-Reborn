#include <stdint.h>

#define ENTERED  0x1270a9e17e5e3001ULL
#define COMPLETE 0x1270a9e17e5e3002ULL
#define TIMEOUT  0x1270a9e17e5e3003ULL
#define MAX_POLLS 1000000U

void vmcb_probe_1270(uint64_t *param)
{
  volatile uint64_t *ctrl =
      (volatile uint64_t *)(uintptr_t)(param[1] + 0x62a75090ULL);
  volatile uint64_t *oracle = (volatile uint64_t *)(uintptr_t)param[2];
  uint64_t value = *ctrl;
  uint64_t first = value;
  uint64_t polls = 0;

  oracle[0] = ENTERED;
  while ((value & ~1ULL) != 8 && polls < MAX_POLLS) {
    __asm__ volatile("pause");
    value = *ctrl;
    ++polls;
  }
  oracle[1] = first;
  oracle[2] = value;
  oracle[3] = polls;
  oracle[0] = (value & ~1ULL) == 8 ? COMPLETE : TIMEOUT;
}
