#!/bin/sh
set -e
cd "$(dirname "$0")"

CC=${HV_CC:-gcc}
LD=${HV_LD:-ld}
OBJCOPY=${HV_OBJCOPY:-objcopy}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -fno-plt \
  -fno-stack-protector -fno-stack-clash-protection \
  -fomit-frame-pointer -fno-asynchronous-unwind-tables \
  -fcf-protection=none -fno-jump-tables -mgeneral-regs-only \
  -mno-red-zone -Wall -Wextra -Werror"

"$CC" $CFLAGS -c break_hv_diag.c -o break_hv_diag.o
"$CC" -c entry.S -o entry.o
"$LD" -nostdlib -no-pie -T link.ld entry.o break_hv_diag.o -o break_hv_diag.elf
"$OBJCOPY" -O binary break_hv_diag.elf break_hv_diag.bin
xxd -i -n break_hv break_hv_diag.bin > break_hv_diag.h
