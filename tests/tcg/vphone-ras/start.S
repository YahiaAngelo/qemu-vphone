// vphone-linux: bare-metal AArch64 test of the TCG return-address stack (VPHONE_RAS).
// Boot as tests/fp/vphone-native-fp/start.S, plus the assembly pieces of the tests.
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
    lsr     x1, x1, #26
    cmp     x1, #0x15                   // SVC64: return at once
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

    .text
// u64 ret_elsewhere(void): the callee returns past the instruction after its call
// (x30 replaced), so the pushed return address is not the target.
    .global ret_elsewhere
ret_elsewhere:
    stp     x29, x30, [sp, #-16]!
    mov     x0, #0
    adr     x1, 1f
    bl      jump_to_x1
    mov     x0, #99                     // must be skipped
1:  add     x0, x0, #5
    ldp     x29, x30, [sp], #16
    ret
jump_to_x1:
    mov     x30, x1
    ret

// int rs_setjmp(u64 buf[14]) / void rs_longjmp(u64 buf[14], int v)
    .global rs_setjmp
rs_setjmp:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    mov     x0, #0
    ret
    .global rs_longjmp
rs_longjmp:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    mov     x0, x1
    ret

// u64 pageend_call(u64 (*f)(u64), u64 v): its BL is the last instruction of a page,
// the return address the first of the next one.
    .section .text.pageend, "ax"
    .balign 4096
    .global pageend_call
pageend_call:
    stp     x29, x30, [sp, #-16]!
    mov     x2, x0
    mov     x0, x1
    b       2f
    .space  4092 - 16
2:  blr     x2                          // offset 4092
    add     x0, x0, #3                  // next page
    ldp     x29, x30, [sp], #16
    ret

// Code copied into RAM by the self-modifying and remapping tests (position
// independent): calls x1, then returns the constant moved after the call.
    .section .rodata.tmpl, "a"
    .balign 16
    .global tmpl_start, tmpl_end, tmpl_movA, tmpl_movB
tmpl_start:
    stp     x29, x30, [sp, #-16]!
    blr     x1
tmpl_movA:
    mov     x0, #1
    ldp     x29, x30, [sp], #16
    ret
tmpl_end:
tmpl_movB:
    mov     x0, #2

// PAC: u64 pac_call(u64 (*f)(void)): signs x30 (PACIBSP) and returns with RETAB.
    .text
    .global pac_call
pac_call:
    .inst   0xd503237f                  // pacibsp
    stp     x29, x30, [sp, #-16]!
    blr     x0
    add     x0, x0, #1
    ldp     x29, x30, [sp], #16
    .inst   0xd65f0fff                  // retab
    .global pac_leaf
pac_leaf:
    .inst   0xd503233f                  // paciasp
    mov     x0, #40
    .inst   0xd65f0bff                  // retaa
