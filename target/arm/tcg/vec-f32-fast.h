/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Included by vec_helper.c, and by tests/fp/vphone-f32-fast.c, which checks
 * vf32_fast() against softfloat lane by lane.
 */
#ifndef TARGET_ARM_VEC_F32_FAST_H
#define TARGET_ARM_VEC_F32_FAST_H

#include "fpu/softfloat.h"

/*
 * vphone-linux: float32 vector arithmetic on the host's SIMD unit.
 *
 * An iOS 27 guest spends about a tenth of QEMU's time in Neon float32
 * multiply-adds, multiplies, adds and subtracts, which the generic helpers
 * evaluate one lane at a time through softfloat's entry points. vf32_fast()
 * computes a whole vector with SSE (and FMA for the fused forms) when the
 * hardware result is exactly the IEEE result softfloat would give and raises
 * no flag that is not already raised, under the same conditions as
 * softfloat's own hardfloat path:
 *
 *  - the status already holds the inexact flag and rounds to nearest-even;
 *  - every input lane is zero or normal (no NaN, infinity or denormal, so
 *    default-NaN, flush-to-zero on input and FPCR.AH have nothing to act on);
 *  - every result lane is finite and either normal and above FLT_MIN, or a
 *    zero that is exact: any sum or difference that is zero is exact, and a
 *    product or fused product-sum may only be zero when a multiplicand is.
 *
 * Under these, the only flag the operation could raise is inexact, which is
 * set already; overflow, underflow and invalid each need a lane that fails a
 * check, and any failing lane sends the whole vector to the generic loop,
 * which has not run and finds its inputs untouched (the results go to a
 * buffer first, as d may alias an operand). VPHONE_FAST_F32=0 turns it off.
 *
 * With native FP (target/arm/tcg/host-fpcr.h) MXCSR holds the guest's
 * rounding mode and accumulates the guest's exception flags while helpers
 * run, so this also requires MXCSR to round to nearest, and puts MXCSR back
 * when it gives up: the lanes it rejects may have raised anything. When it
 * succeeds the only flag it can have raised is inexact, which the status
 * already holds.
 */
#if defined(__x86_64__)
#include <float.h>
#include <immintrin.h>

enum { VF32_ADD, VF32_SUB, VF32_MUL, VF32_FMA, VF32_FMS };

static int vf32_mode = -1;      /* -1 unknown, 0 off, 1 SSE only, 2 SSE + FMA */

static int vf32_init(void)
{
    const char *e = getenv("VPHONE_FAST_F32");
    int mode = 0;

    if (!(e && *e == '0')) {
        mode = __builtin_cpu_supports("fma") ? 2 : 1;
    }
    qatomic_set(&vf32_mode, mode);
    return mode;
}

/* Lanes that are zero or normal. */
static inline __m128 vf32_zon(__m128 v, __m128 abs_mask)
{
    __m128 a = _mm_and_ps(v, abs_mask);
    __m128 normal = _mm_and_ps(_mm_cmpge_ps(a, _mm_set1_ps(FLT_MIN)),
                               _mm_cmple_ps(a, _mm_set1_ps(FLT_MAX)));
    return _mm_or_ps(normal, _mm_cmpeq_ps(v, _mm_setzero_ps()));
}

/* Lanes whose result passes (see above); zero_ok marks the exact zeros. */
static inline __m128 vf32_result_ok(__m128 r, __m128 zero_ok, __m128 abs_mask)
{
    __m128 a = _mm_and_ps(r, abs_mask);
    __m128 big = _mm_and_ps(_mm_cmpgt_ps(a, _mm_set1_ps(FLT_MIN)),
                            _mm_cmple_ps(a, _mm_set1_ps(FLT_MAX)));
    return _mm_or_ps(big, _mm_and_ps(_mm_cmpeq_ps(r, _mm_setzero_ps()),
                                     zero_ok));
}

__attribute__((target("fma")))
static inline __m128 vf32_fmadd(__m128 n, __m128 m, __m128 a)
{
    return _mm_fmadd_ps(n, m, a);
}

/*
 * d = n op m (op ADD, SUB, MUL) or d = a + n * m (FMA) / a - n * m (FMS),
 * oprsz bytes; m[idx] of each 16-byte segment broadcast when idx >= 0.
 * Returns false, having written nothing, when the vector must take the
 * generic loop.
 */
static bool vf32_fast(void *vd, const void *vn, const void *vm, const void *va,
                      float_status *s, intptr_t oprsz, int op, intptr_t idx)
{
    int mode = qatomic_read(&vf32_mode);
    float out[64];

    if (unlikely(mode < 0)) {
        mode = vf32_init();
    }
    if (mode == 0 || (op >= VF32_FMA && mode < 2) ||
        !(s->float_exception_flags & float_flag_inexact) ||
        s->float_rounding_mode != float_round_nearest_even ||
        (oprsz & 7) || oprsz > (intptr_t)sizeof(out)) {
        return false;
    }

    /*
     * The compiler does not order SSE arithmetic against MXCSR accesses:
     * the arithmetic hangs off the pointers, which come out of the asm that
     * reads MXCSR, and the second read takes ok, which every lane's result
     * and every check feeds.
     */
    unsigned csr, now;
    const float *n = vn, *m = vm, *a = va;

    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    if (csr & 0x6000) {         /* MXCSR.RC not round-to-nearest */
        return false;
    }
    __asm__ volatile("" : "+r"(n), "+r"(m), "+r"(a));

    const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(INT32_MAX));
    const __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(INT32_MIN));
    __m128 ok = _mm_castsi128_ps(_mm_set1_epi32(-1));

    for (intptr_t i = 0; i < oprsz / 4; i += 4) {
        bool half = oprsz - i * 4 == 8;     /* a 64-bit vector: two lanes */
        __m128 vn_, vm_, va_ = _mm_setzero_ps(), r, zero_ok;

        if (half) {
            vn_ = _mm_castpd_ps(_mm_load_sd((const double *)(n + i)));
            vm_ = idx >= 0 ? _mm_set1_ps(m[i + idx])
                           : _mm_castpd_ps(_mm_load_sd((const double *)(m + i)));
            if (op >= VF32_FMA) {
                va_ = _mm_castpd_ps(_mm_load_sd((const double *)(a + i)));
            }
        } else {
            vn_ = _mm_loadu_ps(n + i);
            vm_ = idx >= 0 ? _mm_set1_ps(m[i + idx]) : _mm_loadu_ps(m + i);
            if (op >= VF32_FMA) {
                va_ = _mm_loadu_ps(a + i);
            }
        }
        ok = _mm_and_ps(ok, _mm_and_ps(vf32_zon(vn_, abs_mask),
                                       vf32_zon(vm_, abs_mask)));
        switch (op) {
        case VF32_ADD:
            r = _mm_add_ps(vn_, vm_);
            zero_ok = ok;
            break;
        case VF32_SUB:
            r = _mm_sub_ps(vn_, vm_);
            zero_ok = ok;
            break;
        case VF32_MUL:
            r = _mm_mul_ps(vn_, vm_);
            zero_ok = _mm_or_ps(_mm_cmpeq_ps(vn_, _mm_setzero_ps()),
                                _mm_cmpeq_ps(vm_, _mm_setzero_ps()));
            break;
        default: /* FMA, FMS */
            ok = _mm_and_ps(ok, vf32_zon(va_, abs_mask));
            r = vf32_fmadd(op == VF32_FMS ? _mm_xor_ps(vn_, sign) : vn_,
                           vm_, va_);
            zero_ok = _mm_or_ps(_mm_cmpeq_ps(vn_, _mm_setzero_ps()),
                                _mm_cmpeq_ps(vm_, _mm_setzero_ps()));
            break;
        }
        ok = _mm_and_ps(ok, vf32_result_ok(r, zero_ok, abs_mask));
        _mm_storeu_ps(out + i, r);
    }
    if (_mm_movemask_ps(ok) != 0xf) {
        __asm__ volatile("stmxcsr %0" : "=m"(now) : "x"(ok));
        if (now != csr) {
            __asm__ volatile("ldmxcsr %0" :: "m"(csr));
        }
        return false;
    }
    memcpy(vd, out, oprsz);
    return true;
}
#else
enum { VF32_ADD, VF32_SUB, VF32_MUL, VF32_FMA, VF32_FMS };
static inline bool vf32_fast(void *vd, const void *vn, const void *vm,
                             const void *va, float_status *s, intptr_t oprsz,
                             int op, intptr_t idx)
{
    return false;
}
#endif

#endif
