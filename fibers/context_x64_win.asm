; Win64 fiber switch. The frame layout is mirrored by build_initial_frame in
; context.cpp:
;   00h-9Fh xmm6-xmm15, A0h StackBase, A8h StackLimit, B0h DeallocationStack,
;   B8h mxcsr, BCh x87 control word, C8h-100h r15..rbp, 108h return address.
; The TIB stack fields are swapped so that SEH dispatch, __chkstk and the
; kernel's guard-page stack growth see the fiber's stack, not the thread's.

EXTERN nx_fiber_main:PROC

.code

; void *nx_fiber_switch(void **from_sp, void *to_sp, void *data)
nx_fiber_switch PROC FRAME
    push rbp
    .pushreg rbp
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, 0C8h
    .allocstack 0C8h
    movaps [rsp+000h], xmm6
    .savexmm128 xmm6, 000h
    movaps [rsp+010h], xmm7
    .savexmm128 xmm7, 010h
    movaps [rsp+020h], xmm8
    .savexmm128 xmm8, 020h
    movaps [rsp+030h], xmm9
    .savexmm128 xmm9, 030h
    movaps [rsp+040h], xmm10
    .savexmm128 xmm10, 040h
    movaps [rsp+050h], xmm11
    .savexmm128 xmm11, 050h
    movaps [rsp+060h], xmm12
    .savexmm128 xmm12, 060h
    movaps [rsp+070h], xmm13
    .savexmm128 xmm13, 070h
    movaps [rsp+080h], xmm14
    .savexmm128 xmm14, 080h
    movaps [rsp+090h], xmm15
    .savexmm128 xmm15, 090h
    .endprolog

    mov r10, gs:[30h]
    mov rax, [r10+08h]
    mov [rsp+0A0h], rax
    mov rax, [r10+10h]
    mov [rsp+0A8h], rax
    mov rax, [r10+1478h]
    mov [rsp+0B0h], rax
    stmxcsr dword ptr [rsp+0B8h]
    fnstcw word ptr [rsp+0BCh]

    mov [rcx], rsp
    mov rsp, rdx

    mov rax, [rsp+0A0h]
    mov [r10+08h], rax
    mov rax, [rsp+0A8h]
    mov [r10+10h], rax
    mov rax, [rsp+0B0h]
    mov [r10+1478h], rax
    ldmxcsr dword ptr [rsp+0B8h]
    fldcw word ptr [rsp+0BCh]

    movaps xmm6, [rsp+000h]
    movaps xmm7, [rsp+010h]
    movaps xmm8, [rsp+020h]
    movaps xmm9, [rsp+030h]
    movaps xmm10, [rsp+040h]
    movaps xmm11, [rsp+050h]
    movaps xmm12, [rsp+060h]
    movaps xmm13, [rsp+070h]
    movaps xmm14, [rsp+080h]
    movaps xmm15, [rsp+090h]
    mov rax, r8
    add rsp, 0C8h
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    ret
nx_fiber_switch ENDP

; Entered by nx_fiber_switch's ret with the RawTransfer pointer in rax. The
; zero above the return slot terminates stack walks at this frame.
nx_fiber_start PROC FRAME
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov rcx, rax
    call nx_fiber_main
    int 3
nx_fiber_start ENDP

END
