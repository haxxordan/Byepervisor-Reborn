#ifndef OFFSETS_12_70_PARTIAL_H
#define OFFSETS_12_70_PARTIAL_H

// Research values only. This header is deliberately not included by kdlsym.c.
// They are relative to KERNEL_ADDRESS_TEXT_BASE, as in the 13.xx tables.
// Source: kstuff-lite prosper0gdb/offsets/12_70.h (kdata-relative values)
// plus PS5 payload SDK kernel.c (text base = data base - 0xD50000).
#define KERNEL_1270_SYM_PMAP_STORE 0x3B6CFB8ULL
#define KERNEL_1270_SYM_IDT_ARRAY  0x3BD8300ULL
#define KERNEL_1270_SYM_COMMON_TSS 0x3BDAFE0ULL
#define KERNEL_1270_GADGET_IRETQ  0x2B34ADULL

#endif // OFFSETS_12_70_PARTIAL_H
