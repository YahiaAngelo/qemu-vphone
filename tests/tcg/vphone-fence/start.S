// vphone-linux: bare-metal AArch64 boot for the fence test (tests/tcg/vphone-fence).
// CPU 0 runs main(); CPU 1 is started by main() through PSCI CPU_ON at sec_entry
// with x0 = its stack top, and runs secondary_main().
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

    .global sec_entry
sec_entry:
    mov     sp, x0
    ldr     x0, =vectors
    msr     vbar_el1, x0
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    bl      secondary_main
    b       park

// u64 psci_call(u64 fn, u64 a, u64 b, u64 c)
    .global psci_call
psci_call:
    hvc     #0
    ret

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

// The store-buffering pairs: u64 sb_N(u64 *mine, u64 *other): store 1 to *mine,
// then load *other and return it.
    .text
    .global sb_plain, sb_stlr_ldar, sb_dmb, sb_stlr_ldapr, sb_stlr_ldaxr
    .global sb_stlxr_ldar, sb_swpal_ldar, sb_casal_ldar, sb_stlur_ldar, sb_stlr_ldar_w
sb_plain:
    mov     x2, #1
    str     x2, [x0]
    ldr     x0, [x1]
    ret
sb_stlr_ldar:
    mov     x2, #1
    stlr    x2, [x0]
    ldar    x0, [x1]
    ret
sb_stlr_ldar_w:                 // 32-bit forms
    mov     w2, #1
    stlr    w2, [x0]
    ldar    w0, [x1]
    ret
sb_dmb:
    mov     x2, #1
    str     x2, [x0]
    dmb     ish
    ldr     x0, [x1]
    ret
sb_stlr_ldapr:                  // allowed to reorder (RCpc)
    mov     x2, #1
    stlr    x2, [x0]
    ldapr   x0, [x1]
    ret
sb_stlr_ldaxr:
    mov     x2, #1
    stlr    x2, [x0]
    ldaxr   x0, [x1]
    clrex
    ret
sb_stlxr_ldar:
    mov     x2, #1
1:  ldxr    x3, [x0]
    stlxr   w3, x2, [x0]
    cbnz    w3, 1b
    ldar    x0, [x1]
    ret
sb_swpal_ldar:
    mov     x2, #1
    swpal   x2, x3, [x0]
    ldar    x0, [x1]
    ret
sb_casal_ldar:
    mov     x2, #1
    mov     x3, #0
    casal   x3, x2, [x0]
    ldar    x0, [x1]
    ret
sb_stlur_ldar:                  // STLUR (RCPC 8.4), unscaled offset
    mov     x2, #1
    stlur   x2, [x0, #8]
    ldar    x0, [x1]
    ret
