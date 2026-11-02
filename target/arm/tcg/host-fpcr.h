/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host FP control/status synchronisation for native guest FP execution.
 *
 * Ported from Inferno (AppleHax) 2eed395 "arm: native A64 FP / NEON on TCG".
 *
 * While a vCPU thread runs generated code (between cpu_exec_enter and
 * cpu_exec_exit), the x86 MXCSR rounding mode follows FPCR.RMode, so the
 * INDEX_op_*_fp opcodes round like the guest. Exceptions stay masked and
 * flush-to-zero stays off (the translator only uses the native path when
 * FPCR.FZ is clear). The exception flags the native code raises accumulate in
 * MXCSR and are folded into FPSR's cumulative bits whenever FPSR is read and
 * when the thread leaves generated code.
 *
 * Everything here is a no-op unless arm_native_fp_enabled (VPHONE_NATIVE_FP,
 * on by default), and only acts on the current thread's register when called
 * from the vCPU thread that owns env.
 */

#ifndef TARGET_ARM_TCG_HOST_FPCR_H
#define TARGET_ARM_TCG_HOST_FPCR_H

#include "cpu.h"
#include "hw/core/cpu.h"

/* Set once at startup from VPHONE_NATIVE_FP (default on); see cpu.c. */
extern bool arm_native_fp_enabled;

#if defined(__x86_64__) && defined(CONFIG_TCG)
/* Only the rounding mode follows the guest; exceptions masked, no FTZ/DAZ. */
#define ARM_HOST_MXCSR_BASE  0x1f80
#define ARM_HOST_MXCSR_FLAGS 0x3f

static inline uint32_t arm_host_mxcsr_read(void)
{
    uint32_t v;
    __asm__ volatile("stmxcsr %0" : "=m"(v));
    return v;
}

static inline void arm_host_mxcsr_write(uint32_t v)
{
    __asm__ volatile("ldmxcsr %0" :: "m"(v) : "memory");
}

static inline bool arm_host_fp_mine(CPUARMState *env)
{
    return env->host_fp_active && current_cpu == env_cpu(env);
}

static inline uint32_t arm_host_mxcsr_rounding(CPUARMState *env)
{
    switch (extract64(env->vfp.fpcr, 22, 2)) {
    case FPROUNDING_TIEEVEN:
        return 0 << 13;
    case FPROUNDING_NEGINF:
        return 1 << 13;
    case FPROUNDING_POSINF:
        return 2 << 13;
    default:
        return 3 << 13;
    }
}

/* MXCSR's DE (bit 1, denormal operand) has no AArch64 counterpart here. */
static inline uint64_t arm_host_mxcsr_to_fpsr(uint32_t m)
{
    return (m & (1 << 0) ? FPSR_IOC : 0) | (m & (1 << 2) ? FPSR_DZC : 0) |
           (m & (1 << 3) ? FPSR_OFC : 0) | (m & (1 << 4) ? FPSR_UFC : 0) |
           (m & (1 << 5) ? FPSR_IXC : 0);
}

static inline void arm_host_fpcr_enter(CPUARMState *env)
{
    uint32_t want;

    if (!arm_native_fp_enabled) {
        return;
    }
    want = ARM_HOST_MXCSR_BASE | arm_host_mxcsr_rounding(env);
    env->host_fpcr_saved = arm_host_mxcsr_read();
    env->host_fp_active = true;
    if (env->host_fpcr_saved != want) {
        arm_host_mxcsr_write(want);
    }
}

/* Must not change state: the cpreg reset check reads FPSR twice. */
static inline uint64_t arm_host_fpsr_peek(CPUARMState *env)
{
    if (!arm_host_fp_mine(env)) {
        return 0;
    }
    return arm_host_mxcsr_to_fpsr(arm_host_mxcsr_read());
}

static inline void arm_host_fpsr_drain(CPUARMState *env)
{
    uint32_t cur;

    if (!arm_host_fp_mine(env)) {
        return;
    }
    cur = arm_host_mxcsr_read();
    if (!(cur & ARM_HOST_MXCSR_FLAGS)) {
        return;
    }
    env->vfp.fpsr |= arm_host_mxcsr_to_fpsr(cur);
    arm_host_mxcsr_write(cur & ~ARM_HOST_MXCSR_FLAGS);
}

/* The guest wrote FPSR (or reset it): forget what the host accumulated. */
static inline void arm_host_fpsr_ack(CPUARMState *env)
{
    uint32_t cur;

    if (!arm_host_fp_mine(env)) {
        return;
    }
    cur = arm_host_mxcsr_read();
    if (cur & ARM_HOST_MXCSR_FLAGS) {
        arm_host_mxcsr_write(cur & ~ARM_HOST_MXCSR_FLAGS);
    }
}

/* The guest wrote FPCR (or reset it): follow its rounding mode. */
static inline void arm_host_fpcr_reload(CPUARMState *env)
{
    uint32_t cur, want;

    if (!arm_host_fp_mine(env)) {
        return;
    }
    cur = arm_host_mxcsr_read();
    want = (cur & ARM_HOST_MXCSR_FLAGS) | ARM_HOST_MXCSR_BASE |
           arm_host_mxcsr_rounding(env);
    if (cur != want) {
        arm_host_mxcsr_write(want);
    }
}

static inline void arm_host_fpcr_exit(CPUARMState *env)
{
    if (!arm_host_fp_mine(env)) {
        return;
    }
    arm_host_fpsr_drain(env);
    arm_host_mxcsr_write(env->host_fpcr_saved);
    env->host_fp_active = false;
}
#else
static inline void arm_host_fpcr_enter(CPUARMState *env) { }
static inline void arm_host_fpcr_exit(CPUARMState *env) { }
static inline uint64_t arm_host_fpsr_peek(CPUARMState *env) { return 0; }
static inline void arm_host_fpsr_drain(CPUARMState *env) { }
static inline void arm_host_fpsr_ack(CPUARMState *env) { }
static inline void arm_host_fpcr_reload(CPUARMState *env) { }
#endif

#endif
