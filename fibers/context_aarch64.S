// AAPCS64 fiber switch. The frame layout is mirrored by build_initial_frame
// in context.cpp:
//   00h-38h d8-d15, 40h-88h x19-x28, 90h fpcr, A0h x29, A8h x30.

#if defined(__aarch64__)

#if defined(__APPLE__)
#define NX_SYMBOL(name) _##name
#define NX_FUNCTION(name) \
  .private_extern NX_SYMBOL(name) %% .globl NX_SYMBOL(name) %% NX_SYMBOL(name):
#define NX_END_FUNCTION(name)
#else
#define NX_SYMBOL(name) name
#define NX_FUNCTION(name) \
  .hidden name; .globl name; .type name, %function; name:
#define NX_END_FUNCTION(name) .size name, .-name
#endif

.text

// void *nx_fiber_switch(void **from_sp, void *to_sp, void *data)
.p2align 2
NX_FUNCTION(nx_fiber_switch)
  .cfi_startproc
  sub sp, sp, #0xb0
  .cfi_def_cfa_offset 0xb0
  stp x29, x30, [sp, #0xa0]
  .cfi_offset x29, -0x10
  .cfi_offset x30, -0x08
  stp x27, x28, [sp, #0x80]
  .cfi_offset x27, -0x30
  .cfi_offset x28, -0x28
  stp x25, x26, [sp, #0x70]
  .cfi_offset x25, -0x40
  .cfi_offset x26, -0x38
  stp x23, x24, [sp, #0x60]
  .cfi_offset x23, -0x50
  .cfi_offset x24, -0x48
  stp x21, x22, [sp, #0x50]
  .cfi_offset x21, -0x60
  .cfi_offset x22, -0x58
  stp x19, x20, [sp, #0x40]
  .cfi_offset x19, -0x70
  .cfi_offset x20, -0x68
  stp d14, d15, [sp, #0x30]
  .cfi_offset d14, -0x80
  .cfi_offset d15, -0x78
  stp d12, d13, [sp, #0x20]
  .cfi_offset d12, -0x90
  .cfi_offset d13, -0x88
  stp d10, d11, [sp, #0x10]
  .cfi_offset d10, -0xa0
  .cfi_offset d11, -0x98
  stp d8, d9, [sp, #0x00]
  .cfi_offset d8, -0xb0
  .cfi_offset d9, -0xa8
  mrs x9, fpcr
  str x9, [sp, #0x90]

  mov x9, sp
  str x9, [x0]
  mov sp, x1

  // Writing fpcr can stall the pipeline, so skip it when nothing changed.
  ldr x9, [sp, #0x90]
  mrs x10, fpcr
  cmp x9, x10
  b.eq 1f
  msr fpcr, x9
1:
  ldp d8, d9, [sp, #0x00]
  ldp d10, d11, [sp, #0x10]
  ldp d12, d13, [sp, #0x20]
  ldp d14, d15, [sp, #0x30]
  ldp x19, x20, [sp, #0x40]
  ldp x21, x22, [sp, #0x50]
  ldp x23, x24, [sp, #0x60]
  ldp x25, x26, [sp, #0x70]
  ldp x27, x28, [sp, #0x80]
  ldp x29, x30, [sp, #0xa0]
  add sp, sp, #0xb0
  .cfi_def_cfa_offset 0
  mov x0, x2
  ret
  .cfi_endproc
NX_END_FUNCTION(nx_fiber_switch)

// Entered by nx_fiber_switch's ret with the RawTransfer pointer in x0 and
// x29 = 0, which ends frame-pointer walks here.
.p2align 2
NX_FUNCTION(nx_fiber_start)
  .cfi_startproc
  .cfi_undefined x30
  bl NX_SYMBOL(nx_fiber_main)
  brk #0
  .cfi_endproc
NX_END_FUNCTION(nx_fiber_start)

#if defined(__ELF__)
.section .note.GNU-stack, "", %progbits
#endif

#endif
