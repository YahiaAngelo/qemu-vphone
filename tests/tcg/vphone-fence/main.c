/*
 * vphone-linux: bare-metal AArch64 store-buffering litmus test for the
 * acquire/release barrier mapping on an x86 host (VPHONE_FENCE,
 * target/arm/tcg/translate-a64.c).
 *
 * Two vCPUs each store 1 to their own location and then load the other's
 * (both locations start at 0). Both loads returning 0 means a load passed the
 * earlier store, which an x86 host does unless a fence separates them. Arm
 * forbids it when the store is a release (STLR, STLUR, STLXR, SWPAL, CASAL)
 * and the load an RCsc acquire (LDAR, LDAXR), or a DMB ISH lies between; it
 * allows it for a plain pair and for STLR followed by LDAPR (RCpc). Each
 * forbidden pair must show 0 such outcomes; the plain pair must show some,
 * or the test could not see a missing fence. Run on 2 vCPUs (run.sh).
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
static void putd(u64 v)
{
    char b[24];
    int n = 0;
    do {
        b[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n) {
        putc_(b[--n]);
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

u64 psci_call(u64 fn, u64 a, u64 b, u64 c);
void sec_entry(void);

typedef u64 (*sbfn)(volatile u64 *mine, volatile u64 *other);
u64 sb_plain(volatile u64 *, volatile u64 *);
u64 sb_stlr_ldar(volatile u64 *, volatile u64 *);
u64 sb_stlr_ldar_w(volatile u64 *, volatile u64 *);
u64 sb_dmb(volatile u64 *, volatile u64 *);
u64 sb_stlr_ldapr(volatile u64 *, volatile u64 *);
u64 sb_stlr_ldaxr(volatile u64 *, volatile u64 *);
u64 sb_stlxr_ldar(volatile u64 *, volatile u64 *);
u64 sb_swpal_ldar(volatile u64 *, volatile u64 *);
u64 sb_casal_ldar(volatile u64 *, volatile u64 *);
u64 sb_stlur_ldar(volatile u64 *, volatile u64 *);

static const struct {
    const char *name;
    sbfn fn;
    int forbidden;
    int off;            /* the store's offset from the pointer passed */
} V[] = {
    { "plain str;ldr      ", sb_plain, 0, 0 },
    { "stlr;ldar          ", sb_stlr_ldar, 1, 0 },
    { "stlr;ldar (32-bit) ", sb_stlr_ldar_w, 1, 0 },
    { "str;dmb ish;ldr    ", sb_dmb, 1, 0 },
    { "stlr;ldapr         ", sb_stlr_ldapr, 0, 0 },
    { "stlr;ldaxr         ", sb_stlr_ldaxr, 1, 0 },
    { "ldxr/stlxr;ldar    ", sb_stlxr_ldar, 1, 0 },
    { "swpal;ldar         ", sb_swpal_ldar, 1, 0 },
    { "casal;ldar         ", sb_casal_ldar, 1, 0 },
    { "stlur;ldar         ", sb_stlur_ldar, 1, 8 },
};
#define NV (sizeof(V) / sizeof(V[0]))
#define ITERS 2000000

struct line {
    volatile u64 v;
    char pad[120];
} __attribute__((aligned(128)));
static struct line X, Y, start, done, r1;
static volatile u64 variant;
static u64 sec_stack[2048] __attribute__((aligned(16)));

static inline void dmb(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}
static inline u64 next(u64 *r)
{
    *r = *r * 6364136223846793005ULL + 1442695040888963407ULL;
    return *r >> 33;
}
static inline void delay(u64 n)
{
    for (u64 k = n & 31; k; k--) {
        __asm__ volatile("" ::: "memory");
    }
}
static volatile u64 *store_ptr(struct line *l, int off)
{
    return (volatile u64 *)((char *)&l->v - off);
}

void secondary_main(void)
{
    u64 seen = 0, rng = 777;

    for (;;) {
        u64 i;
        while ((i = start.v) == seen) {
        }
        seen = i;
        dmb();
        u64 v = variant;
        delay(next(&rng));
        u64 r = V[v].fn(store_ptr(&Y, V[v].off), &X.v);
        r1.v = r;
        dmb();
        done.v = i;
    }
}

int main(void)
{
    u64 seq = 0, rng = 4242;
    int failures = 0, sensitive = 0;

    puts_("vphone-fence store-buffering test, 2 vCPUs\n");
    if (psci_call(0xC4000003UL, 1, (u64)sec_entry, (u64)(sec_stack + 2048)) != 0) {
        puts_("FAIL CPU_ON\nTOTAL fail\n");
        return 1;
    }
    for (u64 v = 0; v < NV; v++) {
        u64 both0 = 0;
        variant = v;
        for (u64 i = 0; i < ITERS; i++) {
            X.v = 0;
            Y.v = 0;
            dmb();
            start.v = ++seq;
            delay(next(&rng));
            u64 r0 = V[v].fn(store_ptr(&X, V[v].off), &Y.v);
            while (done.v != seq) {
            }
            dmb();
            if (r0 == 0 && r1.v == 0) {
                both0++;
            }
        }
        int bad = V[v].forbidden && both0;
        failures += bad;
        if (!V[v].forbidden && both0) {
            sensitive = 1;
        }
        puts_(bad ? "FAIL " : "ok   ");
        puts_(V[v].name);
        puts_(V[v].forbidden ? " forbidden" : " allowed  ");
        puts_(" both-0 outcomes: ");
        putd(both0);
        puts_("\n");
    }
    if (!sensitive) {
        puts_("INSENSITIVE: no allowed pair reordered\n");
    }
    puts_(failures ? "TOTAL fail\n" : sensitive ? "TOTAL ok\n" : "TOTAL insensitive\n");
    return 0;
}
