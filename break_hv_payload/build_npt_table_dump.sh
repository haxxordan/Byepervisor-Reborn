#!/bin/sh
set -eu
cd "$(dirname "$0")"

"${HV_CC:-gcc}" -c -nostdlib -fcf-protection=none \
  npt_table_dump.S -o npt_table_dump.o
"${HV_OBJCOPY:-objcopy}" -O binary -j .text \
  npt_table_dump.o npt_table_dump.bin
xxd -i -n npt_table_dump npt_table_dump.bin > npt_table_dump.h
