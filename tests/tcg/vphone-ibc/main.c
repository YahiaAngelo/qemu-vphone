/*
 * vphone-linux: bare-metal AArch64 test of the TCG indirect-branch cache
 * (VPHONE_IBC, target/arm/tcg/translate-a64.c vphone_gen_ibc) and of the
 * checked cross-page links' slow way (VPHONE_XPAGE_REFILL).
 *
 * Each test checks its own result and prints a line; run.sh runs it with the
 * cache on and off on the same QEMU and compares. The cases are the ones
 * where a branch site's cached TB is wrong and must not be used: other
 * targets of the same site (BR jump table, function pointers, BRAA/BLRAB/
 * BRAAZ), the target's code rewritten, the target's page remapped (TLBI),
 * its address space switched (ASID), one physical page at two virtual
 * addresses (the TB is reused, its PC-relative results must follow the
 * address), and direct branches and fall-throughs into a page that is
 * remapped (checked cross-page links). Everything runs with the MMU off, then
 * again with it on. Run on a core without BTI (neoverse-v1).
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

/* ---- asm (start.S) ---- */
u64 br_dispatch(u64 sel, u64 x);
u64 br_tail(u64 (*f)(u64), u64 x);
u64 braa_tail(u64 (*f)(u64), u64 x);
u64 blraa_call(u64 (*f)(u64), u64 x);
u64 braaz_tail(u64 (*f)(u64), u64 x);
u64 br_to(u64 va);
u64 blr_to(u64 va);
extern const u32 tmpl_const[], tmpl_const_end[], tmpl_adr[], tmpl_adr_end[];
extern const u32 tmpl_mov2[], tmpl_mov3[], tmpl_mov4[], insn_nop[];

/* ---- function pointers ---- */
static NOINLINE u64 f0(u64 x) { return x + 1; }
static NOINLINE u64 f1(u64 x) { return x * 3; }
static NOINLINE u64 f2(u64 x) { return x ^ 0x5555; }
static NOINLINE u64 f3(u64 x) { return f0(x) + f1(x); }
static NOINLINE u64 f4(u64 x) { return f2(f3(x)); }
static NOINLINE u64 f5(u64 x) { return x >> 1; }
static NOINLINE u64 f6(u64 x) { return f5(f4(x)) + 7; }
static NOINLINE u64 f7(u64 x) { return x - 2; }
static u64 (*const fns[8])(u64) = { f0, f1, f2, f3, f4, f5, f6, f7 };

static u64 ref(int k, u64 x)
{
    switch (k) {
    case 0: return x + 1;
    case 1: return x * 3;
    case 2: return x ^ 0x5555;
    case 3: return (x + 1) + x * 3;
    case 4: return ((x + 1) + x * 3) ^ 0x5555;
    case 5: return x >> 1;
    case 6: return ((((x + 1) + x * 3) ^ 0x5555) >> 1) + 7;
    default: return x - 2;
    }
}

/* a selector that stays the same for a while (cache hits), then changes */
static int pick(int i)
{
    return (i & 0x300) ? (int)(rnd() & 7) : (i >> 10) & 7;
}

typedef u64 (*caller_fn)(u64 (*)(u64), u64);
static NOINLINE u64 via(caller_fn c, int iters, int extra)
{
    u64 acc = 1, accr = 1, save = rng;

    for (int i = 0; i < iters; i++) {
        acc = (c(fns[pick(i)], acc) & 0xffffffffff);
    }
    rng = save;
    for (int i = 0; i < iters; i++) {
        accr = (ref(pick(i), accr) + extra) & 0xffffffffff;
    }
    return acc == accr ? 0 : acc ^ accr;
}

static NOINLINE u64 call_ptr(u64 (*f)(u64), u64 x)
{
    return f(x);
}

static u64 disp_ref(u64 sel, u64 x)
{
    switch (sel & 7) {
    case 0: return x + 1;
    case 1: return x * x;
    case 2: return x ^ 0xff00;
    case 3: return x >> 1;
    case 4: return x - 3;
    case 5: return x << 2;
    case 6: return x + (x >> 3);
    default: return ~x;
    }
}

static NOINLINE u64 dispatch_test(int iters)
{
    u64 a = 7, b = 7, save = rng;

    for (int i = 0; i < iters; i++) {
        a = br_dispatch(pick(i), a) & 0xffffffffff;
    }
    rng = save;
    for (int i = 0; i < iters; i++) {
        b = disp_ref(pick(i), b) & 0xffffffffff;
    }
    return a == b ? 0 : a ^ b;
}

/* ---- code in RAM ---- */
static u32 smc_page[1024] __attribute__((aligned(4096)));
static u32 page_a[1024] __attribute__((aligned(4096)));
static u32 page_b[1024] __attribute__((aligned(4096)));
static u32 page_c[1024] __attribute__((aligned(4096)));
static u32 page_d[1024] __attribute__((aligned(4096)));
static u32 page_e[1024] __attribute__((aligned(4096)));
static u32 page_f[1024] __attribute__((aligned(4096)));

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

static void copy_code(u32 *dst, const u32 *s, const u32 *e)
{
    for (long i = 0; i < e - s; i++) {
        dst[i] = s[i];
    }
    sync_code(dst, (e - s) * 4);
}

/* SMC: the target's first instruction rewritten between branches */
static NOINLINE u64 smc_test(int rounds, u64 (*go)(u64))
{
    u64 bad = 0;

    copy_code(smc_page, tmpl_const, tmpl_const_end);
    for (int i = 0; i < rounds; i++) {
        u64 want = (i & 1) ? 2 : 1;

        smc_page[0] = (i & 1) ? tmpl_mov2[0] : tmpl_const[0];
        sync_code(smc_page, 4);
        for (int j = 0; j < 4; j++) {
            bad += go((u64)smc_page) != want;
        }
    }
    return bad;
}

/* ---- MMU ---- */
#define VA_X 0x80000000ul         /* page A or B (returns 1 or 2) */
#define VA_T (VA_X + 0x4000)      /* page F: nops at 0xff8, a B at 0x800 */
#define VA_T2 (VA_X + 0x5000)     /* page C or D (returns 3 or 4) */
#define VA_Y1 (VA_X + 0x8000)     /* page E (returns its own address) */
#define VA_Y2 (VA_X + 0x9000)     /* page E again */
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
        l3s[t][4] = page_desc(page_f);
        l3s[t][5] = page_desc(page_c);
        l3s[t][8] = page_desc(page_e);
        l3s[t][9] = page_desc(page_e);
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

static void tlbi_va(u64 va)
{
    asm volatile("dsb ishst\n\ttlbi vae1is, %0\n\tdsb ish\n\tisb"
                 :: "r"((va >> 12) | (1ul << 48)) : "memory");
}

/* remap: VA_X to the other page (TLBI) between branches */
static NOINLINE u64 remap_test(int rounds, u64 (*go)(u64))
{
    u64 bad = 0;
    int b = 0;

    for (int i = 0; i < rounds; i++) {
        for (int j = 0; j < 4; j++) {
            bad += go(VA_X) != (b ? 2 : 1);
        }
        b = !b;
        l3_1[0] = page_desc(b ? page_b : page_a);
        tlbi_va(VA_X);
    }
    l3_1[0] = page_desc(page_a);
    tlbi_va(VA_X);
    return bad;
}

/* ASID: TTBR0 switched (ASID 1 maps VA_X to page A, ASID 2 to B), no TLBI */
static NOINLINE u64 asid_test(int rounds, u64 (*go)(u64))
{
    u64 bad = 0;
    int a2 = 0;

    for (int i = 0; i < rounds; i++) {
        for (int j = 0; j < 4; j++) {
            bad += go(VA_X) != (a2 ? 2 : 1);
        }
        a2 = !a2;
        asm volatile("msr ttbr0_el1, %0\n\tisb"
                     :: "r"(a2 ? (u64)l1_2 | (2ul << 48) : (u64)l1_1 | (1ul << 48))
                     : "memory");
    }
    asm volatile("msr ttbr0_el1, %0\n\tisb" :: "r"((u64)l1_1 | (1ul << 48)) : "memory");
    return bad;
}

/* alias: one physical page at two addresses; the code returns its own address */
static NOINLINE u64 alias_test(int rounds, u64 (*go)(u64))
{
    u64 bad = 0;

    for (int i = 0; i < rounds; i++) {
        u64 va = (i & 4) ? VA_Y2 : VA_Y1;
        bad += go(va) != va;
    }
    return bad;
}

/*
 * Direct branches into a remapped page: page F holds nops at its end (the
 * code falls through into VA_T2) and a B to VA_T2 at 0x800; VA_T2 maps page
 * C (returns 3) or page D (returns 4).
 */
static NOINLINE u64 xpage_test(int rounds, u64 off, u64 (*go)(u64))
{
    u64 bad = 0;
    int d = 0;

    for (int i = 0; i < rounds; i++) {
        for (int j = 0; j < 4; j++) {
            bad += go(VA_T + off) != (d ? 4 : 3);
        }
        if (i & 1) {
            d = !d;
            l3_1[5] = page_desc(d ? page_d : page_c);
            tlbi_va(VA_T2);
        }
    }
    l3_1[5] = page_desc(page_c);
    tlbi_va(VA_T2);
    return bad;
}

static void run_all(void)
{
    check("call_ptr", via(call_ptr, 100000, 0), 0);
    check("br_tail", via(br_tail, 100000, 0), 0);
    check("braa_tail", via(braa_tail, 100000, 0), 0);
    check("braaz_tail", via(braaz_tail, 100000, 0), 0);
    check("blrab_call", via(blraa_call, 100000, 1), 0);
    check("br_dispatch", dispatch_test(200000), 0);
    check("smc_blr", smc_test(3000, blr_to), 0);
    check("smc_br", smc_test(3000, br_to), 0);
    if (mmu_on) {
        check("remap_blr", remap_test(5000, blr_to), 0);
        check("remap_br", remap_test(5000, br_to), 0);
        check("asid_blr", asid_test(5000, blr_to), 0);
        check("asid_br", asid_test(5000, br_to), 0);
        check("alias_blr", alias_test(20000, blr_to), 0);
        check("alias_br", alias_test(20000, br_to), 0);
        check("xpage_fall", xpage_test(5000, 0xff8, blr_to), 0);
        check("xpage_b", xpage_test(5000, 0x800, blr_to), 0);
    }
}

int main(void)
{
    copy_code(page_a, tmpl_const, tmpl_const_end);
    copy_code(page_b, tmpl_const, tmpl_const_end);
    page_b[0] = tmpl_mov2[0];
    copy_code(page_c, tmpl_const, tmpl_const_end);
    page_c[0] = tmpl_mov3[0];
    copy_code(page_d, tmpl_const, tmpl_const_end);
    page_d[0] = tmpl_mov4[0];
    copy_code(page_e, tmpl_adr, tmpl_adr_end);
    page_f[0x800 / 4] = 0x14000000 | (0x800 / 4);   /* b . + 0x800 = VA_T2 */
    page_f[0xff8 / 4] = insn_nop[0];
    page_f[0xffc / 4] = insn_nop[0];
    sync_code(page_b, 4);
    sync_code(page_c, 4);
    sync_code(page_d, 4);
    sync_code(page_f, 4096);
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
