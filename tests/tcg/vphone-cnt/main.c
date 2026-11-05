/*
 * vphone-linux: bare-metal AArch64 test of counter register reads
 * (VPHONE_FAST_CNT, target/arm/tcg/translate-a64.c): CNTVCT_EL0/CNTPCT_EL0
 * read in a loop never go backwards and advance; at EL0 they trap while
 * CNTKCTL_EL1.EL0VCTEN/EL0PCTEN are 0 and read once set (the access check
 * is kept); the virtual offset (CNTVOFF_EL2, 0 here) relation holds:
 * CNTPCT - CNTVCT is constant. run.sh runs it with the fast path on and off.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
typedef unsigned long u64;
typedef unsigned int u32;

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

extern volatile u64 trap_esr, trap_count;
u64 run_el0(void (*fn)(void));
void el0_read_vct(void);
void el0_read_pct(void);
u64 vct_loop(u64 n, u64 *bad);

static int failures;
static void check(const char *name, int ok)
{
    puts_(ok ? "ok   " : "FAIL ");
    puts_(name);
    puts_("\n");
    failures += !ok;
}
static inline u64 rd_vct(void) { u64 v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
static inline u64 rd_pct(void) { u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline void wr_cntkctl(u64 v) { __asm__ volatile("msr cntkctl_el1, %0; isb" :: "r"(v)); }

int main(void)
{
    u64 bad = 0, span;

    puts_("vphone-cnt counter read test\n");
    span = vct_loop(2000000, &bad);
    check("cntvct never goes backwards in a straight-line loop", bad == 0);
    check("cntvct advances over 2M reads", span > 0);

    u64 v0 = rd_vct(), p0 = rd_pct();
    for (volatile int i = 0; i < 100000; i++) {
    }
    u64 v1 = rd_vct(), p1 = rd_pct();
    check("cntvct and cntpct advance together (no virtual offset)",
          v1 > v0 && p1 > p0 && (p0 - v0) < 0x1000000 && (p1 - v1) < 0x1000000);

    wr_cntkctl(0);
    trap_count = 0;
    u64 r = run_el0(el0_read_vct);
    check("EL0 cntvct read traps when CNTKCTL_EL1.EL0VCTEN=0",
          trap_count == 1 && (trap_esr >> 26) == 0x18 && r == 0);
    trap_count = 0;
    r = run_el0(el0_read_pct);
    check("EL0 cntpct read traps when CNTKCTL_EL1.EL0PCTEN=0",
          trap_count == 1 && (trap_esr >> 26) == 0x18 && r == 0);
    wr_cntkctl(3);
    trap_count = 0;
    u64 before = rd_vct();
    r = run_el0(el0_read_vct);
    check("EL0 cntvct read works when enabled", trap_count == 0 && r >= before);
    trap_count = 0;
    r = run_el0(el0_read_pct);
    check("EL0 cntpct read works when enabled", trap_count == 0 && r != 0);

    puts_(failures ? "TOTAL fail\n" : "TOTAL ok\n");
    return 0;
}
