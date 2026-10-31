/*
 * vphone-linux: check target/arm/tcg/vec-f32-fast.h against softfloat.
 *
 * For each operation (add, sub, mul, fused multiply-add and -subtract, with
 * and without a by-element operand) on 64-, 128- and 256-bit vectors, random
 * inputs drawn to hit every edge vf32_fast() must refuse (NaN, infinity,
 * denormal, overflow, underflow, inexact zero) run through vf32_fast() and
 * through softfloat lane by lane, as the generic helpers do. Wherever
 * vf32_fast() accepts a vector, its lanes must equal softfloat's bit for bit,
 * and softfloat must have raised no flag the status did not already hold.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "../../target/arm/tcg/vec-f32-fast.h"

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint32_t rnd32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng >> 16;
}

/* An input lane: mostly ordinary normals, often an edge case. */
static uint32_t pick(void)
{
    uint32_t sign = rnd32() & 0x80000000u;

    switch (rnd32() % 16) {
    case 0:  return sign;                                   /* +-0 */
    case 1:  return sign | (rnd32() & 0x007fffffu);         /* denormal */
    case 2:  return sign | 0x7f800000u;                     /* infinity */
    case 3:  return 0x7fc00000u | (rnd32() & 0x3fffff);     /* NaN */
    case 4:  return sign | ((1 + rnd32() % 24) << 23) | (rnd32() & 0x7fffff); /* tiny normal */
    case 5:  return sign | ((230 + rnd32() % 25) << 23) | (rnd32() & 0x7fffff); /* huge */
    case 6:  return sign | (127u << 23);                    /* +-1 */
    default: return sign | ((100 + rnd32() % 56) << 23) | (rnd32() & 0x7fffff);
    }
}

static const char *const names[] = { "add", "sub", "mul", "fma", "fms" };

int main(int argc, char **argv)
{
    long trials = argc > 1 ? atol(argv[1]) : 2000000;
    long accepted[5] = { 0 }, run[5] = { 0 };
    int failures = 0;

    for (long t = 0; t < trials; t++) {
        int op = rnd32() % 5;
        intptr_t oprsz = (intptr_t[]){ 8, 16, 32 }[rnd32() % 3];
        intptr_t idx = rnd32() % 2 ? -1 : (intptr_t)(rnd32() % (oprsz == 8 ? 2 : 4));
        uint32_t n[16], m[16], a[16], d[16], ref[16];
        float_status s = { 0 };

        set_float_rounding_mode(rnd32() % 8 ? float_round_nearest_even
                                             : float_round_to_zero, &s);
        set_float_exception_flags(rnd32() % 8 ? float_flag_inexact : 0, &s);
        set_default_nan_mode(true, &s);
        set_float_default_nan_pattern(0b01000000, &s);   /* as target/arm/cpu.c */
        set_float_2nan_prop_rule(float_2nan_prop_s_ab, &s);
        set_float_3nan_prop_rule(float_3nan_prop_s_cab, &s);
        set_float_infzeronan_rule(float_infzeronan_dnan_never, &s);
        for (int i = 0; i < 16; i++) {
            n[i] = pick();
            m[i] = pick();
            a[i] = pick();
            /* exact cancellations, for the zero rules */
            if (rnd32() % 8 == 0) {
                m[i] = op == VF32_SUB ? n[i] : n[i] ^ 0x80000000u;
            }
            if (rnd32() % 8 == 0 && op >= VF32_FMA) {
                float_status x = s;
                a[i] = float32_mul(n[i], m[i], &x) ^ (op == VF32_FMS ? 0 : 0x80000000u);
            }
            d[i] = ref[i] = 0xdeadbeef;
        }

        /* softfloat, lane by lane, as the generic helpers do */
        float_status rs = s;
        int seg = (oprsz < 16 ? oprsz : 16) / 4;
        for (int i = 0; i < oprsz / 4; i++) {
            uint32_t mm = idx >= 0 ? m[(i / seg) * seg + idx] : m[i];
            switch (op) {
            case VF32_ADD: ref[i] = float32_add(n[i], mm, &rs); break;
            case VF32_SUB: ref[i] = float32_sub(n[i], mm, &rs); break;
            case VF32_MUL: ref[i] = float32_mul(n[i], mm, &rs); break;
            case VF32_FMA: ref[i] = float32_muladd(n[i], mm, a[i], 0, &rs); break;
            case VF32_FMS: ref[i] = float32_muladd(n[i] ^ 0x80000000u, mm, a[i], 0, &rs); break;
            }
        }
        if (idx >= 0 && op != VF32_MUL && op < VF32_FMA) {
            continue;   /* by-element add/sub do not exist */
        }

        float_status fs = s;
        run[op]++;
        if (!vf32_fast(d, n, m, op >= VF32_FMA ? a : NULL, &fs, oprsz, op, idx)) {
            continue;
        }
        accepted[op]++;
        bool bad = memcmp(d, ref, oprsz) != 0 ||
                   get_float_exception_flags(&rs) != get_float_exception_flags(&s);
        if (bad && failures++ < 10) {
            printf("MISMATCH %s oprsz %ld idx %ld flags %#x -> soft %#x\n", names[op],
                   (long)oprsz, (long)idx, get_float_exception_flags(&s),
                   get_float_exception_flags(&rs));
            for (int i = 0; i < oprsz / 4; i++) {
                printf("  lane %d: n %08x m %08x a %08x fast %08x soft %08x\n",
                       i, n[i], m[i], a[i], d[i], ref[i]);
            }
        }
    }
    for (int op = 0; op < 5; op++) {
        printf("%s: %ld vectors, %ld taken by the fast path\n", names[op], run[op],
               accepted[op]);
    }
    printf("%s: %d mismatch(es)\n", failures ? "FAIL" : "OK", failures);
    return failures != 0;
}
