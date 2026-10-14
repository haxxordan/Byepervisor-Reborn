#!/bin/sh
set -eu
cd "$(dirname "$0")"

"${HV_CC:-gcc}" -c -nostdlib -fcf-protection=none \
  ap_arg_probe_1270.S -o ap_arg_probe_1270.o
"${HV_OBJCOPY:-objcopy}" -O binary -j .text \
  ap_arg_probe_1270.o ap_arg_probe_1270.bin
xxd -i -n ap_arg_probe_1270 ap_arg_probe_1270.bin > ap_arg_probe_1270.h
