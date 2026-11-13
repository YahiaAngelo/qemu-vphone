/*
 * Apple GXF/SPRR and PAC emulation for hardware accelerators: `hvc` ABI.
 *
 * Copyright (c) 2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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

#pragma once

/*
 * Hardware accelerators can't apply SPRR or Apple's PAC keys or trap GXF, and
 * not all of them trap IMPLEMENTATION DEFINED registers to the host, so the GXF
 * transitions, IMPLEMENTATION DEFINED and MMU/PAC register accesses, ASID
 * invalidations, lower-EL exception entries and exception returns of the
 * monitors and the kernel are rewritten to `hvc` with this immediate (after
 * m1n1):
 *   [15]    0 = control, 1 = system register access
 *   control:
 *     0x1420 | imm5   genter
 *     0x1400          gexit
 *     0x1300          end of the hypervisor's TLB flush stub
 *     0x1320 | Xt     tlbi aside1{,is,os}{,nxs}, Xt
 *     0x1340          exception entry from a lower EL, the vector's first instruction
 *     0x1341          eret
 *     0x1342          IRQ from the current EL, the vector's first instruction; the host takes the FIQ vector instead
 *                     if it's for a FIQ
 *     0x1343          IRQ from a lower EL, like 0x1342
 *   system register:
 *     [14:6]  AppleVirtVReg, or above APPLE_VIRT_VREG_MAX one the accelerator
 *             emulates, from `apple_virt_impdef_vreg`
 *     [5]     1 = mrs, 0 = msr
 *     [4:0]   Xt
 */
#define APPLE_VIRT_HVC_SYSREG         (1 << 15)
#define APPLE_VIRT_HVC_VREG(imm)      (((imm) >> 6) & 0x1FF)
#define APPLE_VIRT_HVC_READ(imm)      (((imm) >> 5) & 1)
#define APPLE_VIRT_HVC_RT(imm)        ((imm) & 0x1F)
#define APPLE_VIRT_HVC_GENTER         0x1420
#define APPLE_VIRT_HVC_GXF_MASK       0xFFE0
#define APPLE_VIRT_HVC_GEXIT          0x1400
#define APPLE_VIRT_HVC_RESUME         0x1300
#define APPLE_VIRT_HVC_TLBI_ASID      0x1320
#define APPLE_VIRT_HVC_TLBI_ASID_MASK 0xFFE0
#define APPLE_VIRT_HVC_LOWER_ENTRY    0x1340
#define APPLE_VIRT_HVC_ERET           0x1341
#define APPLE_VIRT_HVC_IRQ_ENTRY      0x1342
#define APPLE_VIRT_HVC_IRQ_LOWER      0x1343

typedef enum
{
    APPLE_VIRT_VREG_SPRR_CONFIG_EL1,
    APPLE_VIRT_VREG_GXF_CONFIG_EL1,
    APPLE_VIRT_VREG_GXF_STATUS_EL1,
    APPLE_VIRT_VREG_GXF_ENTER_EL1,
    APPLE_VIRT_VREG_GXF_ABORT_EL1,
    APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0,
    APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0,
    APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1,
    APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1,
    APPLE_VIRT_VREG_ASPSR_GL11,
    APPLE_VIRT_VREG_SP_GL11,
    APPLE_VIRT_VREG_TPIDR_GL11,
    APPLE_VIRT_VREG_VBAR_GL11,
    APPLE_VIRT_VREG_SPSR_GL11,
    APPLE_VIRT_VREG_ESR_GL11,
    APPLE_VIRT_VREG_ELR_GL11,
    APPLE_VIRT_VREG_FAR_GL11,
    APPLE_VIRT_VREG_TTBR0_EL1,
    APPLE_VIRT_VREG_TTBR1_EL1,
    APPLE_VIRT_VREG_TCR_EL1,
    APPLE_VIRT_VREG_SCTLR_EL1,
    APPLE_VIRT_VREG_SP_EL1,
    APPLE_VIRT_VREG_APIAKEYLO_EL1,
    APPLE_VIRT_VREG_APIAKEYHI_EL1,
    APPLE_VIRT_VREG_APIBKEYLO_EL1,
    APPLE_VIRT_VREG_APIBKEYHI_EL1,
    APPLE_VIRT_VREG_APDAKEYLO_EL1,
    APPLE_VIRT_VREG_APDAKEYHI_EL1,
    APPLE_VIRT_VREG_APDBKEYLO_EL1,
    APPLE_VIRT_VREG_APDBKEYHI_EL1,
    APPLE_VIRT_VREG_APGAKEYLO_EL1,
    APPLE_VIRT_VREG_APGAKEYHI_EL1,
    // Only rewritten by the patcher if the accelerator doesn't trap them.
    APPLE_VIRT_VREG_APCTL_EL1,
    APPLE_VIRT_VREG_KERNKEYLO_EL1,
    APPLE_VIRT_VREG_KERNKEYHI_EL1,
    /*
     * vphone-linux: M1-style guarded monitors (vresearch101's SPTM) reach the
     * unguarded world's registers by their EL1 names and their own by
     * S3_6_C15_C10_<n>, the other way round from the `_GL11` names above.
     */
    APPLE_VIRT_VREG_M1_TPIDR_EL1,
    APPLE_VIRT_VREG_M1_VBAR_EL1,
    APPLE_VIRT_VREG_M1_SPSR_EL1,
    APPLE_VIRT_VREG_M1_ELR_EL1,
    APPLE_VIRT_VREG_M1_ESR_EL1,
    APPLE_VIRT_VREG_M1_FAR_EL1,
    APPLE_VIRT_VREG_M1_SP_GL1,
    APPLE_VIRT_VREG_M1_TPIDR_GL1,
    APPLE_VIRT_VREG_M1_VBAR_GL1,
    APPLE_VIRT_VREG_M1_SPSR_GL1,
    APPLE_VIRT_VREG_M1_ASPSR_GL1,
    APPLE_VIRT_VREG_M1_ESR_GL1,
    APPLE_VIRT_VREG_M1_ELR_GL1,
    APPLE_VIRT_VREG_M1_FAR_GL1,
    APPLE_VIRT_VREG_MAX,
} AppleVirtVReg;

#define APPLE_VIRT_VREG_LIMIT (APPLE_VIRT_HVC_VREG(~0) + 1)

#define APPLE_VIRT_SYSREG(op0, op1, crn, crm, op2) (((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

/// Whether the guest's guarded monitors use the M1-style register names (vphone-linux).
extern bool apple_virt_m1_gl_names;

/// vphone-linux: whether the patched traps are IMPLEMENTATION DEFINED system register accesses rather than
/// `hvc`s. On an Apple M1 under KVM an `hvc` occasionally runs without trapping to the hypervisor (seen in
/// the lower-EL vector, the kernel's GXF_STATUS check and its eret), while a trapped system register access
/// in the same place always traps. The access is `msr`/`mrs S3_<op1>_C11_C<crm>_<op2>, X<rt>` (CRn 11, which
/// neither XNU nor the Apple CPU models use), carrying the `hvc`'s 16-bit immediate as
/// read:1 op1:3 crm:4 op2:3 rt:5. Unlike an `hvc`, it exits with the PC still on it.
extern bool apple_virt_trap_sysreg;

/* vphone-linux: whether debug variable `name` is set, read once per call site (the exit paths run
 * hundreds of thousands of times a second). */
#define VR_ENV(name) ({ static int vr_env_; if (!vr_env_) { vr_env_ = getenv(name) ? 1 : 2; } vr_env_ == 1; })

/// The instruction that traps with `imm`: an `hvc`, or its system register form.
static inline uint32_t apple_virt_trap_insn(uint16_t imm)
{
    if (!apple_virt_trap_sysreg) { return 0xD4000002u | ((uint32_t)imm << 5); }
    return 0xD518B000u | ((uint32_t)(imm >> 15) << 21) | ((uint32_t)((imm >> 12) & 7) << 16) |
           ((uint32_t)((imm >> 8) & 15) << 8) | ((uint32_t)((imm >> 5) & 7) << 5) | (imm & 31);
}

/// The immediate of a trapped system register access that `apple_virt_trap_insn` made; false if it isn't one.
static inline bool apple_virt_trap_imm(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2,
                                       bool read, uint32_t rt, uint16_t* imm)
{
    if (!apple_virt_trap_sysreg || op0 != 3 || crn != 11) { return false; }
    *imm = (uint16_t)(((uint32_t)read << 15) | (op1 << 12) | (crm << 8) | (op2 << 5) | rt);
    return true;
}

/// The `AppleVirtVReg` of `S<op0>_<op1>_C<crn>_C<crm>_<op2>`, or -1 if GXF/PAC emulation doesn't handle it.
static inline int apple_virt_vreg(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2)
{
    if (apple_virt_m1_gl_names) {
        switch (APPLE_VIRT_SYSREG(op0, op1, crn, crm, op2)) {
            case APPLE_VIRT_SYSREG(3, 0, 13, 0, 4) : return APPLE_VIRT_VREG_M1_TPIDR_EL1;
            case APPLE_VIRT_SYSREG(3, 0, 12, 0, 0) : return APPLE_VIRT_VREG_M1_VBAR_EL1;
            case APPLE_VIRT_SYSREG(3, 0, 4, 0, 0)  : return APPLE_VIRT_VREG_M1_SPSR_EL1;
            case APPLE_VIRT_SYSREG(3, 0, 4, 0, 1)  : return APPLE_VIRT_VREG_M1_ELR_EL1;
            case APPLE_VIRT_SYSREG(3, 0, 5, 2, 0)  : return APPLE_VIRT_VREG_M1_ESR_EL1;
            case APPLE_VIRT_SYSREG(3, 0, 6, 0, 0)  : return APPLE_VIRT_VREG_M1_FAR_EL1;
            case APPLE_VIRT_SYSREG(3, 6, 15, 10, 0): return APPLE_VIRT_VREG_M1_SP_GL1;
            case APPLE_VIRT_SYSREG(3, 6, 15, 10, 1): return APPLE_VIRT_VREG_M1_TPIDR_GL1;
            case APPLE_VIRT_SYSREG(3, 6, 15, 10, 2): return APPLE_VIRT_VREG_M1_VBAR_GL1;
            case APPLE_VIRT_SYSREG(3, 6, 15, 10, 3): return APPLE_VIRT_VREG_M1_SPSR_GL1;
            case APPLE_VIRT_SYSREG(3, 6, 15, 10, 4): return APPLE_VIRT_VREG_M1_ASPSR_GL1;
            case APPLE_VIRT_SYSREG(3, 6, 15, 10, 5): return APPLE_VIRT_VREG_M1_ESR_GL1;
            case APPLE_VIRT_SYSREG(3, 6, 15, 10, 6): return APPLE_VIRT_VREG_M1_ELR_GL1;
            case APPLE_VIRT_SYSREG(3, 6, 15, 10, 7): return APPLE_VIRT_VREG_M1_FAR_GL1;
            default: break;
        }
    }
    switch (APPLE_VIRT_SYSREG(op0, op1, crn, crm, op2)) {
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 0): return APPLE_VIRT_VREG_SPRR_CONFIG_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 2): return APPLE_VIRT_VREG_GXF_CONFIG_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 8, 0): return APPLE_VIRT_VREG_GXF_STATUS_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 8, 1): return APPLE_VIRT_VREG_GXF_ENTER_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 8, 2): return APPLE_VIRT_VREG_GXF_ABORT_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 5): return APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0;
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 6): return APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0;
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 7): return APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 3, 0): return APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 8, 3): return APPLE_VIRT_VREG_ASPSR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 0): return APPLE_VIRT_VREG_SP_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 1): return APPLE_VIRT_VREG_TPIDR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 2): return APPLE_VIRT_VREG_VBAR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 3): return APPLE_VIRT_VREG_SPSR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 5): return APPLE_VIRT_VREG_ESR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 6): return APPLE_VIRT_VREG_ELR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 7): return APPLE_VIRT_VREG_FAR_GL11;
        case APPLE_VIRT_SYSREG(3, 0, 2, 0, 0) : return APPLE_VIRT_VREG_TTBR0_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 0, 1) : return APPLE_VIRT_VREG_TTBR1_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 0, 2) : return APPLE_VIRT_VREG_TCR_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 1, 0, 0) : return APPLE_VIRT_VREG_SCTLR_EL1;
        case APPLE_VIRT_SYSREG(3, 4, 4, 1, 0) : return APPLE_VIRT_VREG_SP_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 1, 0) : return APPLE_VIRT_VREG_APIAKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 1, 1) : return APPLE_VIRT_VREG_APIAKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 1, 2) : return APPLE_VIRT_VREG_APIBKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 1, 3) : return APPLE_VIRT_VREG_APIBKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 2, 0) : return APPLE_VIRT_VREG_APDAKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 2, 1) : return APPLE_VIRT_VREG_APDAKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 2, 2) : return APPLE_VIRT_VREG_APDBKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 2, 3) : return APPLE_VIRT_VREG_APDBKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 3, 0) : return APPLE_VIRT_VREG_APGAKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 3, 1) : return APPLE_VIRT_VREG_APGAKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 4, 15, 0, 4): return APPLE_VIRT_VREG_APCTL_EL1;
        case APPLE_VIRT_SYSREG(3, 4, 15, 1, 0): return APPLE_VIRT_VREG_KERNKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 4, 15, 1, 1): return APPLE_VIRT_VREG_KERNKEYHI_EL1;
        default                               : return -1;
    }
}

/// Whether FIQs arrive as IRQs, so the patcher has the IRQ vectors ask the host which they are.
bool apple_virt_gic_interrupts(void);
/// Whether the patcher rewrites every access to an IMPLEMENTATION DEFINED register, as the accelerator doesn't trap
/// them.
bool apple_virt_rewrites_impdef(void);
/// The vreg for IMPLEMENTATION DEFINED `sysreg` (an `APPLE_VIRT_SYSREG`) when GXF/PAC emulation doesn't handle it,
/// whose accesses the accelerator emulates; -1 if there are too many such registers.
int apple_virt_impdef_vreg(uint32_t sysreg);
/// The register an `hvc` from `apple_virt_impdef_vreg` accesses; returns false if `imm` isn't one.
bool apple_virt_hvc_impdef(uint16_t imm, uint32_t* op0, uint32_t* op1, uint32_t* crn, uint32_t* crm, uint32_t* op2);
