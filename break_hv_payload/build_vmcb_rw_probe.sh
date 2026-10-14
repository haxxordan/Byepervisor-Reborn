#!/bin/sh
set -eu
cd "$(dirname "$0")"

"${HV_CC:-gcc}" -c -nostdlib -fcf-protection=none \
  vmcb_rw_probe.S -o vmcb_rw_probe.o
"${HV_OBJCOPY:-objcopy}" -O binary -j .text \
  vmcb_rw_probe.o vmcb_rw_probe.bin
xxd -i -n vmcb_rw_probe vmcb_rw_probe.bin > vmcb_rw_probe.h
