/*
 * vphone-linux: bare-metal AArch64 test of the TCG return-address stack
 * (VPHONE_RAS, target/arm/tcg/translate-a64.c, accel/tcg/tb-jmp-cache.h).
 *
 * Each test checks its own result, and prints a line; run.sh runs it with
 * VPHONE_RAS=1 and =0 on the same QEMU and compares. The cases are the ones
 * where a return-stack entry is wrong and must not be used: returns that do
 * not go to the pushed address (x30 replaced, longjmp, deeper than the
 * stack), a call at the end of a page, the return site's code rewritten
 * between the call and the return, and the return site's page remapped
 * (TLBI) or its address space switched (ASID) between the call and the
 * return. Everything runs with the MMU off, then again with it on.
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

static int failures;
static void check(const char *name, u64 got, u64 want)
{
    puts_(got == want ? "ok   " : "FAIL ");
    puts_(name);
    puts_(" ");
    putx(got);
    if (got != want) {
        puts_(" want ");
        putx(want);
        failures++;
    }
    puts_("\n");
}

static u64 rng = 0x2545f4914f6cdd1dull;
static u64 rnd(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545f4914f6cdd1dull;
}

#define NOINLINE __attribute__((noinline))
/*
 * virt's -cpu max has BTI: a BLR sets BTYPE, which is part of the TB state,
 * so a callee's return from the TB its BLR entered never matches its return
 * TB's state (a correct miss; Apple cores have no BTI). Ending a callee with
 * a direct branch puts its return in a TB entered with BTYPE 0, as on iOS.
 */
#define BTYPE0() asm volatile("b 1f\n1:" ::: "memory")

/* ---- recursion, deeper than the stack ---- */
static NOINLINE u64 fib(u64 n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}
static NOINLINE u64 ack(u64 m, u64 n)
{
    if (m == 0) {
        return n + 1;
    }
    if (n == 0) {
        return ack(m - 1, 1);
    }
    return ack(m - 1, ack(m, n - 1));
}
static NOINLINE u64 odd_(u64 n);
static NOINLINE u64 even_(u64 n)
{
    return n == 0 ? 1 : odd_(n - 1) + 2;
}
static NOINLINE u64 odd_(u64 n)
{
    return n == 0 ? 0 : even_(n - 1) * 3 % 1000003;
}

/* ---- indirect calls ---- */
static NOINLINE u64 f0(u64 x) { return x + 1; }
static NOINLINE u64 f1(u64 x) { return x * 3; }
static NOINLINE u64 f2(u64 x) { return x ^ 0x5555; }
static NOINLINE u64 f3(u64 x) { return f0(x) + f1(x); }
static NOINLINE u64 f4(u64 x) { return f2(f3(x)); }
static NOINLINE u64 f5(u64 x) { return x >> 1; }
static NOINLINE u64 f6(u64 x) { return f5(f4(x)) + 7; }
static NOINLINE u64 f7(u64 x) { return x - 2; }
static u64 (*const fns[8])(u64) = { f0, f1, f2, f3, f4, f5, f6, f7 };

static NOINLINE u64 indirect_mix(int iters)
{
    u64 acc = 1;

    for (int i = 0; i < iters; i++) {
        acc = fns[rnd() & 7](acc) & 0xffffffffff;
    }
    return acc;
}
static u64 indirect_ref(int iters)
{
    /* the same computation without calls */
    u64 acc = 1;

    for (int i = 0; i < iters; i++) {
        u64 x = acc, r;
        switch (rnd() & 7) {
        case 0: r = x + 1; break;
        case 1: r = x * 3; break;
        case 2: r = x ^ 0x5555; break;
        case 3: r = (x + 1) + x * 3; break;
        case 4: r = ((x + 1) + x * 3) ^ 0x5555; break;
        case 5: r = x >> 1; break;
        case 6: r = ((((x + 1) + x * 3) ^ 0x5555) >> 1) + 7; break;
        default: r = x - 2; break;
        }
        acc = r & 0xffffffffff;
    }
    return acc;
}

/* ---- returns elsewhere ---- */
u64 ret_elsewhere(void);
int rs_setjmp(u64 *buf);
void rs_longjmp(u64 *buf, int v);
static u64 jb[14];
static NOINLINE void dive(int depth)
{
    if (depth == 0) {
        rs_longjmp(jb, 1);
    }
    dive(depth - 1);
    puts_("unreachable\n");
}
static NOINLINE u64 longjmp_test(int rounds)
{
    volatile u64 n = 0;

    for (int i = 0; i < rounds; i++) {
        if (rs_setjmp(jb) == 0) {
            dive(1 + (i % 40));
        } else {
            n += fib(5);    /* returns after the unwind */
        }
    }
    return n;
}

/* ---- a call at the end of a page ---- */
u64 pageend_call(u64 (*f)(u64), u64 v);

/* ---- PAC returns ---- */
u64 pac_call(u64 (*f)(void));
u64 pac_leaf(void);

/* ---- code copied to RAM ---- */
extern const u32 tmpl_start[], tmpl_end[], tmpl_movA[], tmpl_movB[];
#define TMPL_WORDS (tmpl_end - tmpl_start)
#define MOVA_OFF (tmpl_movA - tmpl_start)

static u32 smc_page[1024] __attribute__((aligned(4096)));
static u32 page_a[1024] __attribute__((aligned(4096)));
static u32 page_b[1024] __attribute__((aligned(4096)));

static void sync_code(void *p, u64 len)
{
    for (u64 a = (u64)p & ~63ul; a < (u64)p + len; a += 64) {
        asm volatile("dc cvau, %0" :: "r"(a) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    for (u64 a = (u64)p & ~63ul; a < (u64)p + len; a += 64) {
        asm volatile("ic ivau, %0" :: "r"(a) : "memory");
    }
    asm volatile("dsb ish\n\tisb" ::: "memory");
}

static void copy_tmpl(u32 *dst, u32 mov)
{
    for (long i = 0; i < TMPL_WORDS; i++) {
        dst[i] = tmpl_start[i];
    }
    dst[MOVA_OFF] = mov;
    sync_code(dst, TMPL_WORDS * 4);
}

typedef u64 (*tmpl_fn)(u64, void (*)(void));
static u64 call_tmpl(u64 va, void (*callee)(void))
{
    return ((tmpl_fn)va)(0, callee);
}

static NOINLINE void nop_callee(void)
{
    asm volatile("" ::: "memory");
    BTYPE0();
}

/* SMC: the callee rewrites the instruction its call returns to */
static u32 smc_next;
static NOINLINE void smc_callee(void)
{
    smc_page[MOVA_OFF] = smc_next;
    sync_code(&smc_page[MOVA_OFF], 4);
    BTYPE0();
}

static NOINLINE u64 smc_test(int rounds)
{
    u32 movA = tmpl_movA[0], movB = tmpl_movB[0];
    u64 bad = 0;

    copy_tmpl(smc_page, movA);
    for (int i = 0; i < rounds; i++) {
        /* a few plain calls first, so the call site's return TB is known */
        for (int j = 0; j < 3; j++) {
            u64 want = smc_page[MOVA_OFF] == movA ? 1 : 2;
            bad += call_tmpl((u64)smc_page, nop_callee) != want;
        }
        smc_next = (i & 1) ? movA : movB;
        bad += call_tmpl((u64)smc_page, smc_callee) != ((i & 1) ? 1 : 2);
    }
    return bad;
}

/* ---- MMU ---- */
#define VA_X 0x80000000ul
#define AF (1ul << 10)
#define SH_IN (3ul << 8)
#define NG (1ul << 11)
#define ATTR_DEV (0ul << 2)
#define ATTR_MEM (1ul << 2)
static u64 l1_1[512] __attribute__((aligned(4096)));
static u64 l1_2[512] __attribute__((aligned(4096)));
static u64 l2_1[512] __attribute__((aligned(4096)));
static u64 l2_2[512] __attribute__((aligned(4096)));
static u64 l3_1[512] __attribute__((aligned(4096)));
static u64 l3_2[512] __attribute__((aligned(4096)));
static int mmu_on;

static u64 page_desc(void *pa)
{
    return (u64)pa | AF | SH_IN | ATTR_MEM | NG | 3;
}

static void mmu_enable(void)
{
    u64 *l1s[2] = { l1_1, l1_2 }, *l2s[2] = { l2_1, l2_2 }, *l3s[2] = { l3_1, l3_2 };
    u64 sctlr;

    for (int t = 0; t < 2; t++) {
        l1s[t][0] = 0x00000000ul | AF | ATTR_DEV | 1;
        l1s[t][1] = 0x40000000ul | AF | SH_IN | ATTR_MEM | 1;
        l1s[t][2] = (u64)l2s[t] | 3;
        l2s[t][0] = (u64)l3s[t] | 3;
    }
    l3_1[0] = page_desc(page_a);
    l3_2[0] = page_desc(page_b);
    asm volatile("dsb ish" ::: "memory");
    asm volatile("msr mair_el1, %0" :: "r"(0xfful << 8));
    /* T0SZ 25, inner/outer WBWA, inner shareable, 4K, EPD1, 40-bit PA */
    asm volatile("msr tcr_el1, %0" :: "r"(25ul | (1ul << 8) | (1ul << 10) |
                                           (3ul << 12) | (1ul << 23) | (2ul << 32)));
    asm volatile("msr ttbr0_el1, %0" :: "r"((u64)l1_1 | (1ul << 48)));
    asm volatile("isb\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1 | (1 << 2) | (1 << 12);
    asm volatile("msr sctlr_el1, %0\n\tisb" :: "r"(sctlr) : "memory");
    mmu_on = 1;
}

static void pac_enable(void)
{
    u64 sctlr;

    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ul << 31) | (1ul << 30);     /* EnIA, EnIB */
    asm volatile("msr sctlr_el1, %0\n\tisb" :: "r"(sctlr) : "memory");
}

/* remap: the callee maps VA_X to the other page (TLBI) */
static int remap_to_b;
static NOINLINE void remap_callee(void)
{
    l3_1[0] = page_desc(remap_to_b ? page_b : page_a);
    asm volatile("dsb ishst\n\ttlbi vae1is, %0\n\tdsb ish\n\tisb"
                 :: "r"((VA_X >> 12) | (1ul << 48)) : "memory");
    BTYPE0();
}

static NOINLINE u64 remap_test(int rounds)
{
    u64 bad = 0;

    for (int i = 0; i < rounds; i++) {
        u64 cur = remap_to_b ? 2 : 1;
        for (int j = 0; j < 3; j++) {
            bad += call_tmpl(VA_X, nop_callee) != cur;
        }
        remap_to_b = !remap_to_b;
        bad += call_tmpl(VA_X, remap_callee) != (remap_to_b ? 2 : 1);
    }
    return bad;
}

/* ASID: the callee switches TTBR0 (ASID 1 maps VA_X to page A, ASID 2 to B) */
static int asid2;
static NOINLINE void asid_callee(void)
{
    asid2 = !asid2;
    asm volatile("msr ttbr0_el1, %0\n\tisb"
                 :: "r"(asid2 ? (u64)l1_2 | (2ul << 48) : (u64)l1_1 | (1ul << 48))
                 : "memory");
    BTYPE0();
}

static NOINLINE u64 asid_test(int rounds)
{
    u64 bad = 0;

    for (int i = 0; i < rounds; i++) {
        for (int j = 0; j < 3; j++) {
            bad += call_tmpl(VA_X, nop_callee) != (asid2 ? 2 : 1);
        }
        u64 want = asid2 ? 1 : 2;   /* the callee switches */
        bad += call_tmpl(VA_X, asid_callee) != want;
    }
    return bad;
}

static void run_all(void)
{
    u64 r;

    check("fib(24)", fib(24), 46368);
    check("ack(2,300)", ack(2, 300), 603);
    check("ack(3,6)", ack(3, 6), 509);
    r = 0;
    for (int i = 0; i < 2000; i++) {
        r += even_(i % 300);
    }
    check("mutual", r, r);  /* compared across runs by run.sh */
    puts_("info mutual ");
    putx(r);
    puts_("\n");
    {
        u64 save = rng, a, b;
        a = indirect_mix(200000);
        rng = save;
        b = indirect_ref(200000);
        check("indirect", a, b);
    }
    r = 0;
    for (int i = 0; i < 100000; i++) {
        r += ret_elsewhere();
    }
    check("ret_elsewhere", r, 500000);
    check("longjmp", longjmp_test(20000), 20000 * 5);
    r = 0;
    for (int i = 0; i < 100000; i++) {
        r += pageend_call(f0, i);
    }
    check("pageend", r, 100000ul * 99999 / 2 + 100000ul * 4);
    r = 0;
    for (int i = 0; i < 100000; i++) {
        r += pac_call(pac_leaf);
    }
    check("pac", r, 100000ul * 41);
    check("smc", smc_test(5000), 0);
    if (mmu_on) {
        check("remap", remap_test(5000), 0);
        check("asid", asid_test(5000), 0);
    }
}

int main(void)
{
    copy_tmpl(page_a, tmpl_movA[0]);
    copy_tmpl(page_b, tmpl_movB[0]);
    pac_enable();
    puts_("== MMU off\n");
    run_all();
    mmu_enable();
    puts_("== MMU on\n");
    run_all();
    puts_(failures ? "TOTAL FAIL " : "TOTAL ok ");
    putx(failures);
    puts_("\n");
    return 0;
}
