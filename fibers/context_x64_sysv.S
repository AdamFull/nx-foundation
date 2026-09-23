// System V x86-64 fiber switch. The frame layout is mirrored by
// build_initial_frame in context.cpp:
//   00h mxcsr, 04h x87 control word, 08h-30h r15..rbp, 38h return address.

#if defined(__x86_64__) && !defined(_WIN32)

#if defined(__APPLE__)
#define NX_SYMBOL(name) _##name
#define NX_FUNCTION(name) \
  .private_extern NX_SYMBOL(name); .globl NX_SYMBOL(name); NX_SYMBOL(name):
#define NX_END_FUNCTION(name)
#define NX_CALL(name) call NX_SYMBOL(name)
#else
#define NX_SYMBOL(name) name
#define NX_FUNCTION(name) \
  .hidden name; .globl name; .type name, @function; name:
#define NX_END_FUNCTION(name) .size name, .-name
#define NX_CALL(name) call name@PLT
#endif

.text

// void *nx_fiber_switch(void **from_sp, void *to_sp, void *data)
.p2align 4
NX_FUNCTION(nx_fiber_switch)
  .cfi_startproc
  pushq %rbp
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %rbp, 0
  pushq %rbx
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %rbx, 0
  pushq %r12
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %r12, 0
  pushq %r13
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %r13, 0
  pushq %r14
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %r14, 0
  pushq %r15
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %r15, 0
  subq $8, %rsp
  .cfi_adjust_cfa_offset 8
  stmxcsr (%rsp)
  fnstcw 4(%rsp)

  movq %rsp, (%rdi)
  movq %rsi, %rsp

  ldmxcsr (%rsp)
  fldcw 4(%rsp)
  addq $8, %rsp
  .cfi_adjust_cfa_offset -8
  popq %r15
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r15
  popq %r14
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r14
  popq %r13
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r13
  popq %r12
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r12
  popq %rbx
  .cfi_adjust_cfa_offset -8
  .cfi_restore %rbx
  popq %rbp
  .cfi_adjust_cfa_offset -8
  .cfi_restore %rbp
  movq %rdx, %rax
  ret
  .cfi_endproc
NX_END_FUNCTION(nx_fiber_switch)

// Entered by nx_fiber_switch's ret with the RawTransfer pointer in rax.
.p2align 4
NX_FUNCTION(nx_fiber_start)
  .cfi_startproc
  .cfi_undefined %rip
  movq %rax, %rdi
  NX_CALL(nx_fiber_main)
  ud2
  .cfi_endproc
NX_END_FUNCTION(nx_fiber_start)

#if defined(__ELF__)
.section .note.GNU-stack, "", %progbits
#endif

#endif
