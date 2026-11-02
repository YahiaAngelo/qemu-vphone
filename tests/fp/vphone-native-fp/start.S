// Bare-metal entry for QEMU virt: EL1 (dropping from EL2 if needed), FP on.
    .section .text.boot, "ax"
    .global _start
_start:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    el1
    mov     x0, #(1 << 31)              // HCR_EL2.RW: EL1 is AArch64
    msr     hcr_el2, x0
    mov     x0, #0x3c5                  // EL1h, DAIF masked
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
    mov     x0, #(3 << 20)              // CPACR_EL1.FPEN: no FP traps
    msr     cpacr_el1, x0
    isb
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:  bl      main
    ldr     x0, =0x84000008             // PSCI SYSTEM_OFF
    hvc     #0
park:
    wfe
    b       park

// SVC returns at once (the test takes exceptions between FP ops); anything else stops.
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
    lsr     x1, x1, #26
    cmp     x1, #0x15                   // SVC64
    b.ne    vec_fatal
    ldp     x0, x1, [sp], #16
    eret
vec_fatal:
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      fatal_exception
    ldr     x0, =0x84000008
    hvc     #0
3:  wfe
    b       3b
