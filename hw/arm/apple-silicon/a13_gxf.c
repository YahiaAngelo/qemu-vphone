/*
 * Apple A13 GXF.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "exec/cputlb.h"
#include "hw/arm/apple-silicon/a13_gxf.h"
#include "target/arm/cpregs.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"

static CPAccessResult access_gxf(CPUARMState *env, const ARMCPRegInfo *ri,
                                 bool isread)
{
    if (arm_is_guarded(env)) {
        return CP_ACCESS_OK;
    }
    return CP_ACCESS_TRAP_EL1;
}

static uint64_t tpidr_el1_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    if (arm_is_guarded(env)) {
        return env->gxf.tpidr_gl[1];
    }
    return env->cp15.tpidr_el[1];
}

static void tpidr_el1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    if (arm_is_guarded(env)) {
        env->gxf.tpidr_gl[1] = value;
    } else {
        env->cp15.tpidr_el[1] = value;
    }
}

static uint64_t vbar_el1_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    if (arm_is_guarded(env)) {
        return env->gxf.vbar_gl[1];
    }
    return raw_read(env, ri);
}

static void vbar_el1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                           uint64_t value)
{
    if (arm_is_guarded(env)) {
        env->gxf.vbar_gl[1] = value & ~0x1FULL;
    } else if (!(env->cp15.vmsa_lock_el1 & VMSA_LOCK_VBAR_EL1)) {
        raw_write(env, ri, value & ~0x1FULL);
    }
}

static uint64_t spsr_el1_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    if (arm_is_guarded(env)) {
        return env->gxf.spsr_gl[1];
    }
    return env->banked_spsr[BANK_SVC];
}

static void spsr_el1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                           uint64_t value)
{
    if (arm_is_guarded(env)) {
        env->gxf.spsr_gl[1] = value;
    } else {
        env->banked_spsr[BANK_SVC] = value;
    }
}

static uint64_t elr_el1_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    if (arm_is_guarded(env)) {
        return env->gxf.elr_gl[1];
    }
    return env->elr_el[1];
}

static void elr_el1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                          uint64_t value)
{
    if (arm_is_guarded(env)) {
        env->gxf.elr_gl[1] = value;
    } else {
        env->elr_el[1] = value;
    }
}

static uint64_t esr_el1_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    if (arm_is_guarded(env)) {
        return env->gxf.esr_gl[1];
    }
    return env->cp15.esr_el[1];
}

static void esr_el1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                          uint64_t value)
{
    if (arm_is_guarded(env)) {
        env->gxf.esr_gl[1] = value;
    } else {
        env->cp15.esr_el[1] = value;
    }
}

static uint64_t far_el1_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    if (arm_is_guarded(env)) {
        return env->gxf.far_gl[1];
    }
    return env->cp15.far_el[1];
}

static void far_el1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                          uint64_t value)
{
    if (arm_is_guarded(env)) {
        env->gxf.far_gl[1] = value;
    } else {
        env->cp15.far_el[1] = value;
    }
}

static void sprr_perm_el0_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                uint64_t value)
{
    // maybe use uint64_t for mask/umask?
    uint64_t perm = raw_read(env, ri);
    uint32_t mask = env->sprr.mprr_el_br_el1[0][0];
    if (arm_current_el(env)) {
        raw_write(env, ri, value);
        return;
    }

    for (int i = 0; i < 16; i++) {
        uint32_t umask = SPRR_MASK_EXTRACT_IDX_ATTR(mask, i);
        uint64_t requested_perm = APRR_EXTRACT_IDX_ATTR(value, i);
        uint64_t orig_perm = APRR_EXTRACT_IDX_ATTR(perm, i);
        uint64_t changed_perm = ((requested_perm ^ orig_perm) & umask);
        uint64_t result_perm = orig_perm;
        /* Only change bits that are set in mask */

        result_perm &= ~changed_perm;
        result_perm |= requested_perm & changed_perm;

        perm &= ~(APRR_ATTR_MASK << APRR_SHIFT_FOR_IDX(i));
        perm |= result_perm << APRR_SHIFT_FOR_IDX(i);
    }
    // what's about bit36?
    // Visual also accidentally found it on his own "TPRO regions should not be writable on entry to dyld"

    raw_write(env, ri, perm);
    if (env->sprr_cfg.unified_perms) {
        env->sprr.sprr_el_br_el1[0][1] = perm;
    }

    tlb_flush_by_mmuidx(env_cpu(env), ARMMMUIdxBit_E10_0);
}

/* S3_6_C15_C1_6: EL0/BR1 table, or the EL1 table in the unified layout */
static uint64_t sprr_c1_6_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return env->sprr_cfg.unified_perms ? env->sprr.sprr_el_br_el1[1][0]
                                   : env->sprr.sprr_el_br_el1[0][1];
}

static void sprr_c1_6_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    if (env->sprr_cfg.unified_perms) {
        env->sprr.sprr_el_br_el1[1][0] = value;
        env->sprr.sprr_el_br_el1[1][1] = value;
    } else {
        env->sprr.sprr_el_br_el1[0][1] = value;
    }
    tlb_flush(env_cpu(env));
}

static void sprr_c1_6_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    env->sprr.sprr_el_br_el1[0][1] = 0;
    env->sprr.sprr_el_br_el1[1][0] =
        env->sprr_cfg.unified_perms ? env->sprr_cfg.el1_perm_reset : 0;
    env->sprr.sprr_el_br_el1[1][1] = env->sprr.sprr_el_br_el1[1][0];
}

/* S3_6_C15_C3_0: EL1/BR1 table, or a register of its own when unified */
static uint64_t sprr_c3_0_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return env->sprr_cfg.unified_perms ? env->sprr.c3_0
                                   : env->sprr.sprr_el_br_el1[1][1];
}

static void sprr_c3_0_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    if (env->sprr_cfg.unified_perms) {
        env->sprr.c3_0 = value;
    } else {
        env->sprr.sprr_el_br_el1[1][1] = value;
        tlb_flush(env_cpu(env));
    }
}

static void sprr_c3_0_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    /* the EL1 table itself is reset with S3_6_C15_C1_6 */
    env->sprr.c3_0 = 0;
}

static uint64_t gxf_cpreg_raw_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return *(uint64_t *)((char *)(env) + (ri)->bank_fieldoffsets[0]);
}

static void gxf_cpreg_raw_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                uint64_t value)
{
    *(uint64_t *)((char *)(env) + (ri)->bank_fieldoffsets[0]) = value;
}

static const ARMCPRegInfo apple_a13_gxf_cp_override_reginfo[] = {
    {
        .name = "TPIDR_EL1",
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 0,
        .opc2 = 4,
        .crn = 13,
        .crm = 0,
        .access = PL1_RW,
        .type = ARM_CP_OVERRIDE,
        .readfn = tpidr_el1_read,
        .writefn = tpidr_el1_write,
        .raw_readfn = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, cp15.tpidr_el[1]),
        .resetvalue = 0,
    },
    {
        .name = "VBAR",
        .state = ARM_CP_STATE_BOTH,
        .opc0 = 3,
        .crn = 12,
        .crm = 0,
        .opc1 = 0,
        .opc2 = 0,
        .access = PL1_RW,
        .type = ARM_CP_OVERRIDE,
        .readfn = vbar_el1_read,
        .writefn = vbar_el1_write,
        .raw_readfn = raw_read,
        .raw_writefn = raw_write,
        .bank_fieldoffsets = { offsetof(CPUARMState, cp15.vbar_s),
                               offsetof(CPUARMState, cp15.vbar_ns) },
        .resetvalue = 0,
    },
    {
        .name = "SPSR_EL1",
        .state = ARM_CP_STATE_AA64,
        .type = ARM_CP_ALIAS | ARM_CP_OVERRIDE,
        .opc0 = 3,
        .opc1 = 0,
        .crn = 4,
        .crm = 0,
        .opc2 = 0,
        .access = PL1_RW,
        .raw_readfn = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, banked_spsr[BANK_SVC]),
        .readfn = spsr_el1_read,
        .writefn = spsr_el1_write,
        .resetvalue = 0,
    },
    {
        .name = "ELR_EL1",
        .state = ARM_CP_STATE_AA64,
        .type = ARM_CP_ALIAS | ARM_CP_OVERRIDE,
        .opc0 = 3,
        .opc1 = 0,
        .crn = 4,
        .crm = 0,
        .opc2 = 1,
        .access = PL1_RW,
        .raw_readfn = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, elr_el[1]),
        .readfn = elr_el1_read,
        .writefn = elr_el1_write,
        .resetvalue = 0,
    },
    {
        .name = "ESR_EL1",
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .crn = 5,
        .crm = 2,
        .opc1 = 0,
        .opc2 = 0,
        .access = PL1_RW,
        .type = ARM_CP_OVERRIDE,
        .accessfn = access_tvm_trvm,
        .raw_readfn = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, cp15.esr_el[1]),
        .readfn = esr_el1_read,
        .writefn = esr_el1_write,
        .resetvalue = 0,
    },
    {
        .name = "FAR_EL1",
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .crn = 6,
        .crm = 0,
        .opc1 = 0,
        .opc2 = 0,
        .type = ARM_CP_OVERRIDE,
        .access = PL1_RW,
        .accessfn = access_tvm_trvm,
        .raw_readfn = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, cp15.far_el[1]),
        .readfn = far_el1_read,
        .writefn = far_el1_write,
        .resetvalue = 0,
    },
};

static const ARMCPRegInfo apple_a13_gxf_cp_reginfo[] = {
    {
        .name = "GXF_CONFIG_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 1,
        .opc2 = 2,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_config_el[1]),
    },
    {
        .name = "GXF_STATUS_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 8,
        .opc2 = 0,
        .access = PL1_R,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_status_el[1]),
    },
    {
        .name = "GXF_ENTER_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 8,
        .opc2 = 1,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_enter_el[1]),
    },
    {
        .name = "GXF_ABORT_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 8,
        .opc2 = 2,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_abort_el[1]),
    },
    {
        .name = "ASPSR_GL11",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 8,
        .opc2 = 3,
        .access = PL1_RW,
        .accessfn = access_gxf,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.aspsr_gl[1]),
    },
    {
        .name = "SP_GL11",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 9,
        .opc2 = 0,
        .access = PL2_RW,
        .accessfn = access_gxf,
        .raw_readfn = gxf_cpreg_raw_read,
        .raw_writefn = gxf_cpreg_raw_write,
        .bank_fieldoffsets = { offsetof(CPUARMState, gxf.sp_gl[1]),
                               offsetof(CPUARMState, sp_el[1]) },
        .resetvalue = 0,
    },
    {
        .name = "TPIDR_GL11",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 9,
        .opc2 = 1,
        .access = PL1_RW,
        .accessfn = access_gxf,
        .raw_readfn = gxf_cpreg_raw_read,
        .raw_writefn = gxf_cpreg_raw_write,
        .bank_fieldoffsets = { offsetof(CPUARMState, gxf.tpidr_gl[1]),
                               offsetof(CPUARMState, cp15.tpidr_el[1]) },
        .resetvalue = 0,
    },
    {
        .name = "VBAR_GL11",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 9,
        .opc2 = 2,
        .access = PL1_RW,
        .accessfn = access_gxf,
        .raw_readfn = gxf_cpreg_raw_read,
        .raw_writefn = gxf_cpreg_raw_write,
        .bank_fieldoffsets = { offsetof(CPUARMState, gxf.vbar_gl[1]),
                               offsetof(CPUARMState, cp15.vbar_el[1]) },
        .resetvalue = 0,
    },
    {
        .name = "SPSR_GL11",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 9,
        .opc2 = 3,
        .access = PL1_RW,
        .accessfn = access_gxf,
        .raw_readfn = gxf_cpreg_raw_read,
        .raw_writefn = gxf_cpreg_raw_write,
        .bank_fieldoffsets = { offsetof(CPUARMState, gxf.spsr_gl[1]),
                               offsetof(CPUARMState, banked_spsr[BANK_SVC]) },
        .resetvalue = 0,
    },
    {
        .name = "ESR_GL11",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 9,
        .opc2 = 5,
        .access = PL1_RW,
        .accessfn = access_gxf,
        .raw_readfn = gxf_cpreg_raw_read,
        .raw_writefn = gxf_cpreg_raw_write,
        .bank_fieldoffsets = { offsetof(CPUARMState, gxf.esr_gl[1]),
                               offsetof(CPUARMState, cp15.esr_el[1]) },
        .resetvalue = 0,
    },
    {
        .name = "ELR_GL11",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 9,
        .opc2 = 6,
        .access = PL1_RW,
        .accessfn = access_gxf,
        .raw_readfn = gxf_cpreg_raw_read,
        .raw_writefn = gxf_cpreg_raw_write,
        .bank_fieldoffsets = { offsetof(CPUARMState, gxf.elr_gl[1]),
                               offsetof(CPUARMState, elr_el[1]) },
        .resetvalue = 0,
    },
    {
        .name = "FAR_GL11",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 9,
        .opc2 = 7,
        .access = PL1_RW,
        .accessfn = access_gxf,
        .raw_readfn = gxf_cpreg_raw_read,
        .raw_writefn = gxf_cpreg_raw_write,
        .bank_fieldoffsets = { offsetof(CPUARMState, gxf.far_gl[1]),
                               offsetof(CPUARMState, cp15.far_el[1]) },
        .resetvalue = 0,
    },
    // TODO: Implement lockdown for these registers to prevent unexpected
    // changes
    {
        .name = "SPRR_CONFIG_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 1,
        .opc2 = 0,
        .access = PL1_RW,
        .resetvalue = 0,
        .readfn = raw_read,
        .writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_config_el[1]),
    },
    {
        .name = "SPRR_CONFIG_EL0",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 1,
        .opc2 = 1,
        .access = PL1_RW,
        .resetvalue = 0,
        .readfn = raw_read,
        .writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_config_el[0]),
    },
    {
        .name = "SPRR_EL0BR0_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 1,
        .opc2 = 5,
        .access = PL0_RW,
        .resetvalue = 0,
        .readfn = raw_read,
        .writefn = sprr_perm_el0_write,
        // is that actually not needed?
        // .raw_readfn = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_el_br_el1[0][0]),
    },
    {
        .name = "SPRR_EL0BR1_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 1,
        .opc2 = 6,
        .access = PL1_RW | PL0_R,
        .type = ARM_CP_NO_RAW,
        .readfn = sprr_c1_6_read,
        .writefn = sprr_c1_6_write,
        .resetfn = sprr_c1_6_reset,
    },
    {
        .name = "SPRR_EL1BR0_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 1,
        .opc2 = 7,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_el_br_el1[1][0]),
    },
    {
        .name = "SPRR_EL1BR1_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 3,
        .opc2 = 0,
        .access = PL1_RW,
        .type = ARM_CP_NO_RAW,
        .readfn = sprr_c3_0_read,
        .writefn = sprr_c3_0_write,
        .resetfn = sprr_c3_0_reset,
    },
    {
        .name = "MPRR_EL0BR0_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 3,
        .opc2 = 1,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_el_br_el1[0][0]),
    },
    {
        .name = "MPRR_EL0BR1_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 3,
        .opc2 = 2,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_el_br_el1[0][1]),
    },
    {
        .name = "MPRR_EL1BR0_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 3,
        .opc2 = 3,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_el_br_el1[1][0]),
    },
    {
        .name = "MPRR_EL1BR1_EL1",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 3,
        .opc2 = 4,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_el_br_el1[1][1]),
    },
    {
        // Written by the GPU coprocessor firmware (armfw_g12p) during SPRR
        // setup, right after the EL0/EL1 BR1 permission tables, with two
        // identical 32-bit halves (0x0000f6d80000f6d8). It is the permission
        // table for page-table entries with descriptor bit 55 set (mappings
        // created by the OS in the GPU's shared address space); see
        // get_phys_addr_lpae(). The name is made up and the meaning inferred.
        .name = "MPRR_S3_6_C15_C3_5",
        .cp = CP_REG_ARM64_SYSREG_CP,
        .state = ARM_CP_STATE_AA64,
        .opc0 = 3,
        .opc1 = 6,
        .crn = 15,
        .crm = 3,
        .opc2 = 5,
        .access = PL1_RW,
        .resetvalue = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_s3_6_c15_c3_5),
    },
};

void apple_gxf_define_override_cpregs(ARMCPU *cpu)
{
    if (arm_feature(&cpu->env, ARM_FEATURE_GXF)) {
        define_arm_cp_regs(cpu, apple_a13_gxf_cp_override_reginfo);
    }
}

/*
 * The GL1-banked registers at their M1-and-later encodings,
 * S3_6_C15_C10_<n> (SP, TPIDR, VBAR, SPSR, ASPSR, ESR, ELR, FAR for n = 0..7,
 * as in Asahi Linux's m1n1), sharing the A13 definitions' state and banking.
 */
void apple_gxf_define_m1_gl_cpregs(ARMCPU *cpu)
{
    if (!arm_feature(&cpu->env, ARM_FEATURE_GXF)) {
        return;
    }
    for (int i = 0; i < ARRAY_SIZE(apple_a13_gxf_cp_reginfo); i++) {
        const ARMCPRegInfo *a13 = &apple_a13_gxf_cp_reginfo[i];
        ARMCPRegInfo m1 = *a13;

        if (a13->opc1 == 6 && a13->crn == 15 && a13->crm == 9) {
            m1.crm = 10;
        } else if (a13->opc1 == 6 && a13->crn == 15 && a13->crm == 8 &&
                   a13->opc2 == 3) {
            m1.crm = 10;
            m1.opc2 = 4;
        } else {
            continue;
        }
        /*
         * The A13 entries bank between the GL and EL1 copies by security
         * state, which on a CPU without EL3 always selects the EL1 copy.
         * These encodings name the GL1 register outright.
         */
        if (m1.bank_fieldoffsets[0]) {
            m1.fieldoffset = m1.bank_fieldoffsets[0];
            m1.bank_fieldoffsets[0] = 0;
            m1.bank_fieldoffsets[1] = 0;
            m1.raw_readfn = NULL;
            m1.raw_writefn = NULL;
            m1.readfn = NULL;
            m1.writefn = NULL;
        }
        /* SP_GL1 included: a GL1 handler saves it from EL1 */
        m1.access = PL1_RW;
        m1.name = g_strdup_printf("%s_M1", a13->name);
        define_one_arm_cp_reg(cpu, &m1);
    }
}

void apple_gxf_define_cpregs(ARMCPU *cpu)
{
    if (arm_feature(&cpu->env, ARM_FEATURE_GXF)) {
        define_arm_cp_regs(cpu, apple_a13_gxf_cp_reginfo);
    }
}

void apple_a13_init_gxf_override(AppleA13State *cpu)
{
    apple_gxf_define_override_cpregs(&cpu->parent_obj);
}

void apple_a13_init_gxf(AppleA13State *cpu)
{
    apple_gxf_define_cpregs(&cpu->parent_obj);
}
