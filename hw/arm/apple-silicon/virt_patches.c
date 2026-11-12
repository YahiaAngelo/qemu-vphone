/*
 * ChefKiss Virtualisation Patches.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/arm/apple-silicon/virt_patches.h"
#include "hw/arm/apple_virt_hvc.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"

#define NOP (0xD503201F)

#define CK_VIRT_HVC(imm) (0xD4000002 | ((uint32_t)(imm) << 5))

// What `ck_virt_patch` rewrites to `hvc`, for a hardware accelerator to emulate.
typedef enum
{
    // genter and gexit.
    CK_VIRT_GXF = BIT(0),
    // GXF/SPRR/MMU system-register accesses and ASID invalidations, to run on a guarded monitor's shadow tables.
    CK_VIRT_MONITOR = BIT(1),
    // Lower-EL exception entries, exception returns and PAC key/SCTLR accesses, to give each EL Apple's PAC keys.
    CK_VIRT_PAC = BIT(2),
    // Code of a vCPU GXF/PAC emulation doesn't manage (the SEP's).
    CK_VIRT_UNMANAGED = BIT(3),
    // vphone-linux: a standalone M1-style monitor's (SPTM's, TXM's) EL1 and GL1 register names, never the kernel's.
    CK_VIRT_M1_MONITOR = BIT(4),
} CKVirtFlags;

// What a register access is rewritten for.
static CKVirtFlags ck_virt_vreg_flags(int vreg)
{
    switch (vreg) {
        case APPLE_VIRT_VREG_APIAKEYLO_EL1 ... APPLE_VIRT_VREG_APGAKEYHI_EL1: return CK_VIRT_PAC;
        // SCTLR_EL1 holds both the MMU's and the PAC keys' enables.
        case APPLE_VIRT_VREG_SCTLR_EL1    : return CK_VIRT_PAC | CK_VIRT_MONITOR;
        case APPLE_VIRT_VREG_APCTL_EL1    :
        case APPLE_VIRT_VREG_KERNKEYLO_EL1:
        case APPLE_VIRT_VREG_KERNKEYHI_EL1: return 0;
        case APPLE_VIRT_VREG_M1_TPIDR_EL1 ... APPLE_VIRT_VREG_M1_FAR_GL1: return CK_VIRT_M1_MONITOR;
        default                           : return CK_VIRT_MONITOR;
    }
}

// Adjusts the instruction at `off` of a vector slot for moving it one instruction down, unless its PC-relative target
// is in the slot too; returns false if it can't.
static bool ck_virt_slot_relocate(uint32_t* insn, uint32_t off)
{
    uint32_t w = *insn;
    int      pos, len, scale;
    int32_t  disp;

    if ((w & 0x7C000000) == 0x14000000) {
        // b, bl
        pos = 0, len = 26, scale = 4;
    }
    else if ((w & 0xFF000010) == 0x54000000 || (w & 0x7E000000) == 0x34000000 || (w & 0x3B000000) == 0x18000000) {
        // b.cond, cbz/cbnz, ldr (literal)
        pos = 5, len = 19, scale = 4;
    }
    else if ((w & 0x7E000000) == 0x36000000) {
        // tbz/tbnz
        pos = 5, len = 14, scale = 4;
    }
    else if ((w & 0x9F000000) == 0x10000000) {
        // adr
        disp = (sextract32(w, 5, 19) << 2) | extract32(w, 29, 2);
        if (off + disp < 0x80) { return true; }
        disp  -= 4;
        *insn  = deposit32(deposit32(w, 29, 2, disp & 3), 5, 19, disp >> 2);
        return true;
    }
    else {
        // Not PC-relative, or adrp, whose page doesn't change within a slot.
        return true;
    }

    disp = sextract32(w, pos, len) * scale;
    if (off + disp < 0x80) { return true; }
    disp = disp / scale - 1;
    if (disp < -(1 << (len - 1))) { return false; }
    *insn = deposit32(w, pos, len, disp);
    return true;
}

// Inserts `insn` at the start of the vector slot at `slot`, moving its code down into the NOP that ends it; returns
// whether it could.
static bool ck_virt_slot_insert(uint8_t* slot, uint32_t insn)
{
    uint32_t code[32];

    if ((uint32_t)ldl_le_p(slot + 0x7C) != NOP) { return false; }
    for (int i = 0; i < 31; i++) {
        code[i + 1] = ldl_le_p(slot + i * 4);
        if (!ck_virt_slot_relocate(&code[i + 1], i * 4)) { return false; }
    }
    code[0] = insn;
    for (int i = 0; i < 32; i++) { stl_le_p(slot + i * 4, code[i]); }
    return true;
}

// Vector tables are 2KiB aligned and each AArch64 slot in use is `b <handler>; nop; ...`, or with `inline_slots` (the
// SEP's) the handler padded with NOPs; the synchronous exception slots of the current and the lower EL are always in
// use. XNU's low vectors are `b .` and never taken.
static bool ck_virt_is_vector_table(const uint8_t* table, bool inline_slots)
{
    static const int used[] = {0, 4, 8, 9, 10, 11};

    for (int i = 0; i < ARRAY_SIZE(used); i++) {
        const uint8_t* slot  = table + used[i] * 0x80;
        uint32_t       first = ldl_le_p(slot);

        if (inline_slots ?
                first == 0 || first == NOP || (uint32_t)ldl_le_p(slot + 0x7C) != NOP :
                (first & 0xFC000000) != 0x14000000 || (first & 0x3FFFFFF) == 0
                    // vphone-linux: vresearch101's SPTM pads its last slot with zeros rather than NOPs.
                    || ((uint32_t)ldl_le_p(slot + 4) != NOP && ldl_le_p(slot + 4) != 0))
        {
            return false;
        }
    }
    return true;
}

// Makes the lower-EL AArch64 slots of the vector tables in `range`, mapped at `va`, trap before branching to their
// handlers, and the IRQ slots ask the host whether they're for a FIQ; returns how many.
static uint32_t ck_virt_vectors(CKPatcherRange* range, vaddr va, CKVirtFlags flags)
{
    uint32_t entries = 0;
    bool     gic     = apple_virt_gic_interrupts();

    if (!(flags & CK_VIRT_PAC) && !gic && !getenv("VR_CATCH_CORRUPT")) { return 0; }

    for (vaddr off = -va & 0x7FF; off + 0x800 <= range->length; off += 0x800) {
        if (!ck_virt_is_vector_table(range->ptr + off, flags & CK_VIRT_UNMANAGED)) { continue; }

        // IRQs from the current EL with SP0 and SPx, and from a lower AArch64 EL.
        for (int slot = 1; gic && slot < 12; slot += 4) {
            uint8_t* insn = range->ptr + off + slot * 0x80;
            uint32_t hvc  = CK_VIRT_HVC(slot == 9 ? APPLE_VIRT_HVC_IRQ_LOWER : APPLE_VIRT_HVC_IRQ_ENTRY);

            // The SEP's image is patched again whenever it's resumed.
            if ((uint32_t)ldl_le_p(insn) != hvc && ck_virt_slot_insert(insn, hvc)) { ++entries; }
        }

        // vphone-linux debug: VR_CATCH_CORRUPT also traps FIQs from the current EL (the timers), so the
        // corruption catcher (target/arm/kvm.c) sees where they interrupt. The host does nothing for them.
        // Slots 1 and 2 (IRQ and FIQ, SP0), 5 and 6 (IRQ and FIQ, SPx).
        for (int slot = 1; getenv("VR_CATCH_CORRUPT") && !(flags & CK_VIRT_UNMANAGED) && slot < 7;
             slot += (slot == 2 ? 3 : 1)) {
            uint8_t* insn = range->ptr + off + slot * 0x80;
            uint32_t hvc  = CK_VIRT_HVC(APPLE_VIRT_HVC_IRQ_ENTRY);

            if ((uint32_t)ldl_le_p(insn) != hvc && ck_virt_slot_insert(insn, hvc)) { ++entries; }
        }

        // vphone-linux debug: and the synchronous exceptions from the current EL (slots 0 and 4).
        for (int slot = 0; getenv("VR_CATCH_CORRUPT") && !(flags & CK_VIRT_UNMANAGED) && slot < 8; slot += 4) {
            uint8_t* insn = range->ptr + off + slot * 0x80;
            uint32_t hvc  = CK_VIRT_HVC(0x13fd);

            if ((uint32_t)ldl_le_p(insn) != hvc && ck_virt_slot_insert(insn, hvc)) { ++entries; }
        }

        // The branch moves one instruction down.
        for (int slot = 8; (flags & CK_VIRT_PAC) && slot < 12; slot++) {
            uint8_t* insn = range->ptr + off + slot * 0x80;

            if (gic && slot == 9) { continue; }
            // vphone-linux debug (VR_CATCH_CORRUPT): `hvc 0x1340; hvc 0x13fc; b handler`, the host skipping the
            // second, which only an entry that skipped the first reaches (target/arm/kvm.c).
            if (getenv("VR_CATCH_CORRUPT") && !(flags & CK_VIRT_UNMANAGED) &&
                (uint32_t)ldl_le_p(insn + 8) == NOP && (uint32_t)ldl_le_p(insn + 12) == NOP) {
                // `isb` first when VR_VECTOR_ISB is set (a test of the first instruction after the entry).
                int pre = getenv("VR_VECTOR_ISB") ? 1 : 0;
                uint32_t b = ldl_le_p(insn);

                stl_le_p(insn + 8 + pre * 4, 0x14000000 | ((b - 2 - pre) & 0x3FFFFFF));
                stl_le_p(insn + 4 + pre * 4, CK_VIRT_HVC(0x13fc));
                stl_le_p(insn + pre * 4, CK_VIRT_HVC(APPLE_VIRT_HVC_LOWER_ENTRY));
                if (pre) { stl_le_p(insn, 0xD5033FDF); }
                ++entries;
                continue;
            }
            stl_le_p(insn + 4, 0x14000000 | ((ldl_le_p(insn) - 1) & 0x3FFFFFF));
            stl_le_p(insn, CK_VIRT_HVC(APPLE_VIRT_HVC_LOWER_ENTRY));
            ++entries;
        }
    }
    return entries;
}

static void ck_virt_range(CKPatcherRange* range, vaddr va, CKVirtFlags flags)
{
    uint32_t transitions    = 0;
    uint32_t tlbis          = 0;
    uint32_t entries        = ck_virt_vectors(range, va, flags);
    uint32_t erets          = 0;
    uint32_t sysregs        = 0;
    uint32_t unindexed      = 0;
    bool     rewrite_impdef = apple_virt_rewrites_impdef();

    for (vaddr i = 0; i + sizeof(uint32_t) <= range->length; i += sizeof(uint32_t)) {
        uint8_t* ptr  = range->ptr + i;
        uint32_t insn = ldl_le_p(ptr);
        bool     read = extract32(insn, 21, 1);
        uint32_t rt   = extract32(insn, 0, 5);
        uint32_t op0  = 2 + extract32(insn, 19, 1);
        uint32_t op1  = extract32(insn, 16, 3);
        uint32_t crn  = extract32(insn, 12, 4);
        uint32_t crm  = extract32(insn, 8, 4);
        uint32_t op2  = extract32(insn, 5, 3);
        bool     impdef;
        int      vreg;

        // genter #?, gexit
        if ((flags & CK_VIRT_GXF) && (insn & 0xFFFFFFC0) == 0x00201400) {
            stl_le_p(ptr, CK_VIRT_HVC(insn & 0xFFFF));
            ++transitions;
            continue;
        }

        // tlbi aside1{,is,os}{,nxs}, x?
        if ((flags & CK_VIRT_MONITOR) && (insn & 0xFFFFE0E0) == 0xD5088040
            && (extract32(insn, 8, 4) == 1 || extract32(insn, 8, 4) == 3 || extract32(insn, 8, 4) == 7))
        {
            stl_le_p(ptr, CK_VIRT_HVC(APPLE_VIRT_HVC_TLBI_ASID | rt));
            ++tlbis;
            continue;
        }

        // eret
        if ((flags & CK_VIRT_PAC) && insn == 0xD69F03E0) {
            stl_le_p(ptr, CK_VIRT_HVC(APPLE_VIRT_HVC_ERET));
            ++erets;
            continue;
        }

        // msr/mrs (register)
        if ((insn & 0xFFD00000) != 0xD5100000) { continue; }
        impdef = rewrite_impdef && op0 == 3 && (crn == 11 || crn == 15);
        vreg   = (flags & CK_VIRT_UNMANAGED) ? -1 : apple_virt_vreg(op0, op1, crn, crm, op2);
        if (vreg < 0 && impdef) {
            vreg = apple_virt_impdef_vreg(APPLE_VIRT_SYSREG(op0, op1, crn, crm, op2));
            if (vreg < 0) {
                ++unindexed;
                continue;
            }
        }
        if (vreg < 0 || !(impdef || (flags & ck_virt_vreg_flags(vreg)))) { continue; }

        // `mrs xzr, ?` only serialises.
        if (read && rt == 31 && vreg < APPLE_VIRT_VREG_MAX) { stl_le_p(ptr, NOP); }
        else {
            stl_le_p(ptr, CK_VIRT_HVC(APPLE_VIRT_HVC_SYSREG | (vreg << 6) | (read << 5) | rt));
        }
        ++sysregs;
    }

    if (unindexed != 0) {
        warn_report("`trap to the hypervisor` left %u IMPLEMENTATION DEFINED register accesses in `%s`: out of vregs.",
                    unindexed, range->name);
    }
    if (transitions + tlbis + entries + erets + sysregs == 0) { return; }
    info_report("`trap to the hypervisor` rewrote %u GXF transitions, %u TLBIs, %u exception entries, %u erets and %u "
                "sysregs in `%s`.",
                transitions, tlbis, entries, erets, sysregs, range->name);
}

// Mach-O section attributes: the section holds (only, or some) machine instructions.
#define CK_S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define CK_S_ATTR_SOME_INSTRUCTIONS 0x00000400u

static void ck_virt_patch(MachoHeader64* hdr, CKVirtFlags flags)
{
    vaddr             base;
    MachoHeader64*    img = hdr;
    MachoLoadCommand* cmd;
    uint8_t*          buf;

    apple_boot_get_kc_bounds(hdr, NULL, &base, NULL, NULL, NULL);
    buf = apple_boot_get_macho_buffer(hdr);

    // vphone-linux: a fileset is patched where iBoot loaded it, so its kernel is found by VM address.
    if (hdr->file_type == MH_FILESET) {
        MachoFilesetEntryCommand* fe = apple_boot_get_fileset(hdr, "com.apple.kernel");

        if (fe == NULL) { return; }
        img = (MachoHeader64*)(buf + fe->vm_addr - base);
    }

    // vphone-linux: every code section of every executable segment. SPTM keeps its boot code, the MMU's
    // enable among it, in `__TEXT,__text`, which the segment names alone would miss.
    cmd = (MachoLoadCommand*)(img + 1);
    for (uint32_t i = 0; i < img->n_cmds; i++, cmd = (MachoLoadCommand*)((uint8_t*)cmd + cmd->cmd_size)) {
        MachoSegmentCommand64* seg = (MachoSegmentCommand64*)cmd;
        MachoSection64*        sect;

        if (cmd->cmd != LC_SEGMENT_64 || !(seg->maxprot & VM_PROT_EXECUTE)) { continue; }
        sect = (MachoSection64*)(seg + 1);
        for (uint32_t k = 0; k < seg->nsects; k++, sect++) {
            g_autofree CKPatcherRange* range = NULL;
            g_autofree char*           name  = NULL;

            if (sect->size == 0
                || !(sect->flags & (CK_S_ATTR_PURE_INSTRUCTIONS | CK_S_ATTR_SOME_INSTRUCTIONS)))
            {
                continue;
            }
            name  = g_strdup_printf("%.16s,%.16s", sect->seg_name, sect->sect_name);
            range = ck_patcher_range_from_ptr(name, buf + sect->addr - base, sect->size);
            ck_virt_range(range, sect->addr, flags);
        }
    }
}

void ck_patch_virt(MachoHeader64* hdr) { ck_virt_patch(hdr, CK_VIRT_GXF | CK_VIRT_PAC); }

void ck_patch_virt_unmanaged(CKPatcherRange* range, vaddr va) { ck_virt_range(range, va, CK_VIRT_UNMANAGED); }

void ck_patch_virt_monitor_range(CKPatcherRange* range, vaddr va)
{
    ck_virt_range(range, va, CK_VIRT_GXF | CK_VIRT_PAC | CK_VIRT_MONITOR);
}

void ck_patch_virt_monitor(MachoHeader64* hdr) { ck_virt_patch(hdr, CK_VIRT_GXF | CK_VIRT_PAC | CK_VIRT_MONITOR); }

void ck_patch_virt_m1_monitor(MachoHeader64* hdr)
{
    ck_virt_patch(hdr, CK_VIRT_GXF | CK_VIRT_PAC | CK_VIRT_MONITOR | CK_VIRT_M1_MONITOR);
}
