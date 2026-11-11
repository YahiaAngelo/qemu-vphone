/*
 * Apple GXF/SPRR and PAC emulation for hardware accelerators.
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

#include "exec/hwaddr.h"

typedef enum
{
    APPLE_VIRT_REG_PC,
    APPLE_VIRT_REG_CPSR,
    APPLE_VIRT_REG_SP_EL0,
    APPLE_VIRT_REG_SP_EL1,
    APPLE_VIRT_REG_TPIDR_EL1,
    APPLE_VIRT_REG_VBAR_EL1,
    APPLE_VIRT_REG_SPSR_EL1,
    APPLE_VIRT_REG_ELR_EL1,
    APPLE_VIRT_REG_ESR_EL1,
    APPLE_VIRT_REG_FAR_EL1,
    APPLE_VIRT_REG_SCTLR_EL1,
    APPLE_VIRT_REG_TCR_EL1,
    APPLE_VIRT_REG_TTBR0_EL1,
    APPLE_VIRT_REG_TTBR1_EL1,
    APPLE_VIRT_REG_APIAKEYLO_EL1,
    APPLE_VIRT_REG_APIAKEYHI_EL1,
    APPLE_VIRT_REG_APIBKEYLO_EL1,
    APPLE_VIRT_REG_APIBKEYHI_EL1,
    APPLE_VIRT_REG_APDAKEYLO_EL1,
    APPLE_VIRT_REG_APDAKEYHI_EL1,
    APPLE_VIRT_REG_APDBKEYLO_EL1,
    APPLE_VIRT_REG_APDBKEYHI_EL1,
    APPLE_VIRT_REG_APGAKEYLO_EL1,
    APPLE_VIRT_REG_APGAKEYHI_EL1,
} AppleVirtReg;

/// The register accessors work on a vCPU whose state isn't dirty, straight on the accelerator.
typedef struct
{
    uint64_t (*get_xreg)(CPUState* cs, int n);
    void     (*set_xreg)(CPUState* cs, int n, uint64_t value);
    void     (*get_qreg)(CPUState* cs, int n, uint64_t value[2]);
    uint64_t (*get_reg)(CPUState* cs, AppleVirtReg reg);
    void     (*set_reg)(CPUState* cs, AppleVirtReg reg, uint64_t value);
    /// Changes whether the guest may write [page, page + size) of its memory.
    bool (*protect)(hwaddr page, uint64_t size, bool writable);
    /// Maps host memory read-only at [ipa, ipa + size) of the guest, executable if `exec`.
    void (*map)(void* host, hwaddr ipa, uint64_t size, bool exec);
    /// Makes software-step exceptions exit to the host, or restores the accelerator's own setting; NULL if it can't.
    void (*trap_debug)(CPUState* cs, bool enable);
    /// Accesses to IMPLEMENTATION DEFINED registers trap to the host, so the patcher leaves those it doesn't need.
    bool traps_impdef;
    /// FIQs arrive as IRQs too, which the accelerator tells apart in `APPLE_VIRT_HVC_IRQ_*`.
    bool gic_interrupts;
} AppleVirtOps;

/// Called by an accelerator that can run GXF/PAC emulation, whose guest physical address space ends at `ipa_end`.
void apple_virt_set_ops(const AppleVirtOps* ops, hwaddr ipa_end);
/// Lets GXF/PAC emulation manage a vCPU that runs the patched images.
void apple_virt_manage_cpu(CPUState* cs);

/// Emulates an `hvc` the patcher left without syncing the vCPU state, if it can; returns false if
/// `apple_virt_handle_hvc` is needed.
bool apple_virt_handle_hvc_fast(CPUState* cs, uint16_t imm);
/// Emulates a trapped access to an Apple system register without syncing the vCPU state, if it can; returns false if
/// the accelerator's own handling is needed.
bool apple_virt_handle_sysreg_fast(CPUState* cs, uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2,
                                   bool read, uint32_t rt);
/// After the accelerator emulated a trapped write to a system register: reloads the shadows if it was an SPRR one,
/// which leaves the vCPU after the `msr`; returns whether it did.
bool apple_virt_sysreg_written(CPUState* cs, uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2);
/// Emulates an `hvc` the patcher left; the vCPU state must be synced.
bool apple_virt_handle_hvc(CPUState* cs, uint16_t imm);
/// Handles a store at `va` to a write-protected guest page table at `ipa`; returns whether it was one.
bool apple_virt_handle_write_fault(CPUState* cs, uint64_t va, hwaddr ipa);
/// Resyncs the shadows after a store stepped over by `apple_virt_handle_write_fault`; returns whether it was one.
bool apple_virt_handle_step(CPUState* cs);
/// Around moving the vCPU state between `env` and the accelerator, both ways: in between, `env` holds what the vCPU
/// runs with instead of the guest's state (the current world's banked registers, the current EL's PAC keys and
/// enables, and the shadow tables).
void apple_virt_sync_begin(CPUState* cs);
void apple_virt_sync_end(CPUState* cs);
