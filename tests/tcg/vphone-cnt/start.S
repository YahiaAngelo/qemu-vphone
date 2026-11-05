// vphone-linux: bare-metal AArch64 boot for the counter test (tests/tcg/vphone-cnt).
// Synchronous exceptions from EL0 (current EL with SP_EL0 / lower EL) are recorded in
// trap_esr/trap_count and return past the trapping instruction.
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
    .if \n == 8                     // lower EL, AArch64, synchronous
    b       el0_sync
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

// EL0 bodies: read the counter once (returns it; 0 stays if the read trapped and was skipped)
    .global el0_read_vct, el0_read_pct
el0_read_vct:
    mov     x0, #0
    mrs     x0, cntvct_el0
    svc     #0
el0_read_pct:
    mov     x0, #0
    mrs     x0, cntpct_el0
    svc     #0

// u64 vct_loop(u64 n, u64 *bad): reads CNTVCT n times within straight-line code,
// counts reads lower than the previous one in *bad, returns last - first
    .global vct_loop
vct_loop:
    mrs     x2, cntvct_el0
    mov     x4, x2
    mov     x5, #0
1:  mrs     x3, cntvct_el0
    add     x6, x6, #1
    mrs     x7, cntpct_el0
    cmp     x3, x2
    cinc    x5, x5, lo
    mov     x2, x3
    subs    x0, x0, #1
    b.ne    1b
    str     x5, [x1]
    sub     x0, x2, x4
    ret

    .bss
    .balign 16
    .global trap_esr, trap_count
trap_esr:   .quad 0
trap_count: .quad 0
el0_result: .quad 0
el1_resume: .quad 0
el1_sp:     .quad 0
    .balign 16
    .space 4096
el0_stack_top:
