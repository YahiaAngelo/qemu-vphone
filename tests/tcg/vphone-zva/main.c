/*
 * vphone-linux: bare-metal AArch64 test of DC ZVA (VPHONE_INLINE_ZVA,
 * target/arm/tcg/translate-a64.c vphone_gen_zva): the aligned 64-byte block
 * around the operand is zeroed and nothing else, for any operand offset, with
 * the MMU off and on; a page clear by DC ZVA every 64 bytes; DCZID_EL0; a DC
 * ZVA to an unmapped page and to a read-only page takes a data abort (EC 0x25,
 * WnR) whose FAR is the operand's own address, and writes nothing; at EL0 it
 * traps while SCTLR_EL1.DZE is 0 (EC 0x18) and zeroes once DZE is set.
 * run.sh runs it with the inline path on and off and compares.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

#define UART0 0x09000000UL
static void putc_(char c)
{
    volatile u32 *uart = (volatile u32 *)UART0;
    while (uart[0x18 / 4] & (1 << 5)) {
    }
    uart[0] = (u32)c;
}
static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            putc_('\r');
        }
        putc_(*s++);
    }
}
static void putx(u64 v)
{
    for (int i = 60; i >= 0; i -= 4) {
        putc_("0123456789abcdef"[(v >> i) & 15]);
    }
}
void fatal_exception(u64 vec, u64 esr, u64 elr, u64 far)
{
    puts_("\nFATAL exception esr=");
    putx(esr);
    puts_(" elr=");
    putx(elr);
    puts_(" far=");
    putx(far);
    puts_("\n");
}

extern volatile u64 trap_esr, trap_far, trap_count, el0_arg;
u64 run_el0(void (*fn)(void));
void el0_zva(void);
void zva(void *p);
void zva_range(void *p, u64 len);

static int failures;
static const char *prefix = "";
static void check(const char *name, int ok)
{
    puts_(ok ? "ok   " : "FAIL ");
    puts_(prefix);
    puts_(name);
    puts_("\n");
    failures += !ok;
}

static u8 buf[1024] __attribute__((aligned(4096)));
static u8 page_rw[16384] __attribute__((aligned(16384)));
static u8 page_ro[16384] __attribute__((aligned(16384)));

static void fill(u8 *p, u64 n, u8 v)
{
    for (u64 i = 0; i < n; i++) {
        ((volatile u8 *)p)[i] = v;
    }
}
/* bytes [lo, hi) of p must be 0, the rest of n must be v */
static int pattern_ok(const u8 *p, u64 n, u64 lo, u64 hi, u8 v)
{
    for (u64 i = 0; i < n; i++) {
        u8 want = (i >= lo && i < hi) ? 0 : v;
        if (((volatile const u8 *)p)[i] != want) {
            return 0;
        }
    }
    return 1;
}

static void block_tests(const char *mode)
{
    static const u64 offs[] = { 256, 257, 271, 319, 320, 400, 511, 575 };
    int ok = 1;

    for (unsigned k = 0; k < sizeof(offs) / sizeof(offs[0]); k++) {
        u64 off = offs[k];
        u64 lo = off & ~63ul;
        fill(buf, sizeof(buf), 0xa5);
        zva(buf + off);
        ok &= pattern_ok(buf, sizeof(buf), lo, lo + 64, 0xa5);
    }
    prefix = mode;
    check(": DC ZVA zeroes exactly the aligned 64-byte block, 8 offsets", ok);

    fill(page_rw, sizeof(page_rw), 0x3c);
    zva_range(page_rw + 4096, 8192);
    check(": DC ZVA page clear zeroes 8 KiB and nothing around it",
          pattern_ok(page_rw, sizeof(page_rw), 4096, 12288, 0x3c));
    prefix = "";
}

/* ---- MMU: RAM identity-mapped (1 GiB block), a window at 0x80000000 of 4 KiB pages ---- */
#define VA_W 0x80000000ul
#define AF (1ul << 10)
#define SH_IN (3ul << 8)
#define AP_RO (2ul << 6)
#define ATTR_DEV (0ul << 2)
#define ATTR_MEM (1ul << 2)
static u64 l1[512] __attribute__((aligned(4096)));
static u64 l2[512] __attribute__((aligned(4096)));
static u64 l3[512] __attribute__((aligned(4096)));

static void mmu_enable(void)
{
    u64 sctlr;

    l1[0] = 0x00000000ul | AF | ATTR_DEV | 1;
    l1[1] = 0x40000000ul | AF | SH_IN | ATTR_MEM | 1;
    l1[2] = (u64)l2 | 3;
    l2[0] = (u64)l3 | 3;
    l3[0] = (u64)page_rw | AF | SH_IN | ATTR_MEM | 3;            /* VA_W: read-write */
    l3[1] = (u64)page_ro | AF | SH_IN | ATTR_MEM | AP_RO | 3;    /* VA_W + 4K: read-only */
    l3[2] = 0;                                                   /* VA_W + 8K: unmapped */
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("msr mair_el1, %0" :: "r"(0xfful << 8));
    __asm__ volatile("msr tcr_el1, %0" :: "r"(25ul | (1ul << 8) | (1ul << 10) |
                                              (3ul << 12) | (1ul << 23) | (2ul << 32)));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((u64)l1));
    __asm__ volatile("isb\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1 | (1 << 2) | (1 << 12);
    __asm__ volatile("msr sctlr_el1, %0\n\tisb" :: "r"(sctlr) : "memory");
}

static void set_dze(int on)
{
    u64 sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr = on ? sctlr | (1ul << 14) : sctlr & ~(1ul << 14);
    __asm__ volatile("msr sctlr_el1, %0\n\tisb" :: "r"(sctlr) : "memory");
}

int main(void)
{
    u64 dczid;

    ((volatile u32 *)UART0)[0x30 / 4] = 0x301;     /* UARTCR: enable, TX, RX */
    puts_("vphone-zva DC ZVA test\n");
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
    check("DCZID_EL0 = 4 (64-byte blocks, DC ZVA permitted)", dczid == 4);

    block_tests("MMU off");

    set_dze(0);
    fill(buf, sizeof(buf), 0x77);
    el0_arg = (u64)(buf + 300);
    trap_count = 0;
    u64 r = run_el0(el0_zva);
    check("EL0 DC ZVA traps while SCTLR_EL1.DZE=0 and writes nothing",
          r == 1 && trap_count == 1 && (trap_esr >> 26) == 0x18 &&
          pattern_ok(buf, sizeof(buf), 0, 0, 0x77));
    set_dze(1);
    trap_count = 0;
    r = run_el0(el0_zva);
    check("EL0 DC ZVA zeroes the block once DZE=1",
          r == 1 && trap_count == 0 && pattern_ok(buf, sizeof(buf), 256, 320, 0x77));

    mmu_enable();
    block_tests("MMU on");

    fill(page_rw, sizeof(page_rw), 0x5a);
    zva((void *)(VA_W + 0x1c3));
    check("DC ZVA through a mapped window page zeroes the block",
          pattern_ok(page_rw, sizeof(page_rw), 0x1c0, 0x200, 0x5a));

    trap_count = 0;
    zva((void *)(VA_W + 0x2000 + 0x123));
    check("DC ZVA on an unmapped page: data abort, WnR, FAR = operand",
          trap_count == 1 && (trap_esr >> 26) == 0x25 && (trap_esr & (1 << 6)) &&
          trap_far == VA_W + 0x2000 + 0x123);

    fill(page_ro, sizeof(page_ro), 0x6b);
    trap_count = 0;
    zva((void *)(VA_W + 0x1000 + 0x2f1));
    check("DC ZVA on a read-only page: permission fault, WnR, FAR = operand, nothing written",
          trap_count == 1 && (trap_esr >> 26) == 0x25 && (trap_esr & (1 << 6)) &&
          ((trap_esr & 0x3c) == 0x0c) && trap_far == VA_W + 0x1000 + 0x2f1 &&
          pattern_ok(page_ro, sizeof(page_ro), 0, 0, 0x6b));

    puts_(failures ? "TOTAL fail\n" : "TOTAL ok\n");
    return 0;
}
