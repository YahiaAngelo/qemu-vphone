// vphone-linux: bare-metal AArch64 test of the TCG indirect-branch cache (VPHONE_IBC).
// Boot as tests/tcg/vphone-ras/start.S, plus the assembly pieces of the tests.
    .section .text.boot, "ax"
    .global _start
_start:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    el1
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, el1
    msr     elr_el2, x0
    eret
el1:
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, park
    ldr     x0, =__stack_top
    mov     sp, x0
    ldr     x0, =vectors
    msr     vbar_el1, x0
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:  bl      main
    ldr     x0, =0x84000008
    hvc     #0
park:
    wfe
    b       park

    .balign 2048
vectors:
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .balign 128
    stp     x0, x1, [sp, #-16]!
    mov     x0, #\n
    b       vec_common
    .endr
vec_common:
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      fatal_exception
    ldr     x0, =0x84000008
    hvc     #0
3:  wfe
    b       3b

    .text
// u64 br_dispatch(u64 sel, u64 x): one BR site, eight targets (a jump table)
    .global br_dispatch
br_dispatch:
    and     x0, x0, #7
    adr     x2, 1f
    add     x2, x2, x0, lsl #3
    mov     x0, x1
    br      x2
1:  add     x0, x0, #1
    ret
    mul     x0, x0, x0
    ret
    eor     x0, x0, #0xff00
    ret
    lsr     x0, x0, #1
    ret
    sub     x0, x0, #3
    ret
    lsl     x0, x0, #2
    ret
    add     x0, x0, x0, lsr #3
    ret
    mvn     x0, x0
    ret

// u64 br_tail(u64 (*f)(u64), u64 x): tail call through BR x16
    .global br_tail
br_tail:
    mov     x16, x0
    mov     x0, x1
    br      x16

// u64 braa_tail(u64 (*f)(u64), u64 x): signs f (key A, modifier sp) and tail-calls it with BRAA
    .global braa_tail
braa_tail:
    mov     x16, x0
    mov     x17, sp
    pacia   x16, x17
    mov     x0, x1
    braa    x16, x17

// u64 blraa_call(u64 (*f)(u64), u64 x): signs f (key B, modifier 42), calls it with BLRAB
    .global blraa_call
blraa_call:
    stp     x29, x30, [sp, #-16]!
    mov     x16, x0
    mov     x17, #42
    pacib   x16, x17
    mov     x0, x1
    blrab   x16, x17
    add     x0, x0, #1
    ldp     x29, x30, [sp], #16
    ret

// u64 braaz_tail(u64 (*f)(u64), u64 x): signs f (key A, zero modifier), BRAAZ
    .global braaz_tail
braaz_tail:
    mov     x16, x0
    paciza  x16
    mov     x0, x1
    braaz   x16

// u64 br_to(u64 va): BR to va (a template, which returns to our caller)
    .global br_to
br_to:
    br      x0

// u64 blr_to(u64 va): BLR to va
    .global blr_to
blr_to:
    stp     x29, x30, [sp, #-16]!
    blr     x0
    ldp     x29, x30, [sp], #16
    ret

// Templates copied into RAM (position independent).
    .section .rodata.tmpl, "a"
    .balign 16
    .global tmpl_const, tmpl_const_end, tmpl_adr, tmpl_adr_end
tmpl_const:                             // returns its first instruction's immediate
    mov     x0, #1
    ret
tmpl_const_end:
tmpl_adr:                               // returns its own address
    adr     x0, tmpl_adr
    ret
tmpl_adr_end:
    .global tmpl_mov2, tmpl_mov3, tmpl_mov4, insn_nop
tmpl_mov2:
    mov     x0, #2
tmpl_mov3:
    mov     x0, #3
tmpl_mov4:
    mov     x0, #4
insn_nop:
    nop
