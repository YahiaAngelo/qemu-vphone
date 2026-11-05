// vphone-linux: bare-metal AArch64 boot for the DC ZVA test (tests/tcg/vphone-zva).
// Synchronous exceptions from EL0, and from EL1 (current EL, SPx), are recorded in
// trap_esr/trap_far/trap_count and return past the trapping instruction.
    .section .text.boot, "ax"
    .global _start
_start:
    mrs     x19, CurrentEL
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
2:  ldr     x0, =boot_el
    str     x19, [x0]
    bl      main
    ldr     x0, =0x84000008
    cmp     x19, #(2 << 2)            // booted at EL2: PSCI is SMC (virtualization=on)
    b.eq    4f
    hvc     #0
4:  smc     #0
park:
    wfe
    b       park

    .balign 2048
vectors:
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .balign 128
    .if \n == 8                     // lower EL, AArch64, synchronous
    b       el0_sync
    .elseif \n == 4                 // current EL, SPx, synchronous
    b       el1_sync
    .elseif \n == 9 || \n == 10 || \n == 11
    eret
    .else
    stp     x0, x1, [sp, #-16]!
    mov     x0, #\n
    b       vec_common
    .endif
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

el1_sync:
    stp     x9, x10, [sp, #-16]!
    stp     x11, x12, [sp, #-16]!
    mrs     x9, esr_el1
    ldr     x10, =trap_esr
    str     x9, [x10]
    mrs     x9, far_el1
    ldr     x10, =trap_far
    str     x9, [x10]
    ldr     x10, =trap_count
    ldr     x11, [x10]
    add     x11, x11, #1
    str     x11, [x10]
    mrs     x9, elr_el1
    add     x9, x9, #4
    msr     elr_el1, x9
    ldp     x11, x12, [sp], #16
    ldp     x9, x10, [sp], #16
    eret
el0_sync:
    mrs     x9, esr_el1
    lsr     x10, x9, #26
    cmp     x10, #0x15              // SVC: return to EL1 (x0 = result)
    b.eq    el0_svc
    ldr     x10, =trap_esr
    str     x9, [x10]
    ldr     x10, =trap_count
    ldr     x11, [x10]
    add     x11, x11, #1
    str     x11, [x10]
    mrs     x9, elr_el1
    add     x9, x9, #4
    msr     elr_el1, x9
    eret
el0_svc:
    ldr     x10, =el0_result
    str     x0, [x10]
    ldr     x10, =el1_resume
    ldr     x10, [x10]
    ldr     x11, =el1_sp
    ldr     x11, [x11]
    mov     sp, x11
    br      x10

// u64 run_el0(void (*fn)(void)): runs fn at EL0 (MMU off); fn ends with svc #0 and x0 = result
    .text
    .global run_el0
run_el0:
    stp     x29, x30, [sp, #-16]!
    ldr     x9, =el1_sp
    mov     x10, sp
    str     x10, [x9]
    ldr     x9, =el1_resume
    adr     x10, 1f
    str     x10, [x9]
    msr     elr_el1, x0
    mov     x9, #0                  // EL0t, interrupts unmasked doesn't matter (none enabled)
    msr     spsr_el1, x9
    ldr     x9, =el0_stack_top
    msr     sp_el0, x9
    eret
1:  ldp     x29, x30, [sp], #16
    ldr     x0, =el0_result
    ldr     x0, [x0]
    ret

// EL0 body: DC ZVA at the address in el0_arg, then svc (x0 = 1)
    .global el0_zva
el0_zva:
    ldr     x1, =el0_arg
    ldr     x1, [x1]
    dc      zva, x1
    mov     x0, #1
    svc     #0

// void zva(void *p)
    .global zva
zva:
    dc      zva, x0
    ret

// void zva_range(void *p, u64 len): DC ZVA every 64 bytes (a kernel page clear)
    .global zva_range
zva_range:
1:  dc      zva, x0
    add     x0, x0, #64
    subs    x1, x1, #64
    b.hi    1b
    ret

    .bss
    .balign 16
    .global trap_esr, trap_count, trap_far, el0_arg
trap_esr:   .quad 0
trap_far:   .quad 0
boot_el:    .quad 0
trap_count: .quad 0
el0_arg:    .quad 0
el0_result: .quad 0
el1_resume: .quad 0
el1_sp:     .quad 0
    .balign 16
    .space 4096
el0_stack_top:
