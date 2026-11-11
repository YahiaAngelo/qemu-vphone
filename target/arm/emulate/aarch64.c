/*
 * ARM AARCH64 Fallback Emulation.
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
#include "exec/target_page.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "target/arm/emulate/aarch64.h"
#include "cpu-qom.h"
#include "cpu.h"

/*
 * Emulates one A64 load or store that the accelerator couldn't complete
 * itself: an MMIO access without a valid syndrome (a pair, a writeback, a
 * SIMD register), or one to memory the accelerator doesn't map.
 *
 * Covered: LDR/STR (unsigned offset, unscaled, pre/post-indexed, register
 * offset) of every size, including the sign-extending loads and SIMD&FP
 * registers, and LDP/STP/LDNP/STNP/LDPSW (signed offset, pre/post-indexed)
 * of general-purpose and SIMD&FP registers. Register 31 is SP as a base and
 * XZR as a data register. The vCPU state is synced first, so `env` is
 * current and is written back by the accelerator.
 */

static hwaddr arm_aarch64_fallback_emu_vtop(CPUState *cpu, vaddr addr)
{
    return cpu_get_phys_page_debug(cpu, addr & TARGET_PAGE_MASK) +
           (addr & ~TARGET_PAGE_MASK);
}

typedef struct {
    CPUState *cpu;
    AddressSpace *as;
    ArmAarch64FallbackEmuGetReg get_reg;
    ArmAarch64FallbackEmuSetReg set_reg;
} EmuCtx;

/* Moves `len` bytes (1-16) between the guest's `addr` and `buf`. */
static bool emu_access(EmuCtx *c, vaddr addr, void *buf, unsigned len,
                       bool is_write)
{
    uint8_t *p = buf;

    /* Device registers take at most 8 bytes at a time. */
    while (len > 0) {
        unsigned n = MIN(len, 8);
        hwaddr pa = arm_aarch64_fallback_emu_vtop(c->cpu, addr);
        MemTxResult r;

        if (pa == -1) {
            return false;
        }
        r = is_write
            ? address_space_write(c->as, pa, MEMTXATTRS_UNSPECIFIED, p, n)
            : address_space_read(c->as, pa, MEMTXATTRS_UNSPECIFIED, p, n);
        if (r != MEMTX_OK) {
            return false;
        }
        addr += n;
        p += n;
        len -= n;
    }
    return true;
}

static uint64_t emu_get_base(EmuCtx *c, int rn)
{
    return rn == 31 ? cpu_env(c->cpu)->xregs[31] : c->get_reg(c->cpu, rn);
}

static void emu_set_base(EmuCtx *c, int rn, uint64_t v)
{
    if (rn == 31) {
        cpu_env(c->cpu)->xregs[31] = v;
    } else {
        c->set_reg(c->cpu, rn, v);
    }
}

/* Stores register `rt` (`size` bytes; `simd` for V registers) at `addr`. */
static bool emu_store(EmuCtx *c, vaddr addr, int rt, unsigned size, bool simd)
{
    uint8_t buf[16] = { 0 };

    if (simd) {
        uint64_t *q = aa64_vfp_qreg(cpu_env(c->cpu), rt);

        stq_le_p(buf, q[0]);
        stq_le_p(buf + 8, q[1]);
    } else {
        stq_le_p(buf, c->get_reg(c->cpu, rt));
    }
    return emu_access(c, addr, buf, size, true);
}

/*
 * Loads `size` bytes at `addr` into register `rt`. A general-purpose load
 * sign-extends to `sext` bits (32 or 64) when nonzero; a SIMD load clears the
 * rest of the vector register.
 */
static bool emu_load(EmuCtx *c, vaddr addr, int rt, unsigned size, bool simd,
                     unsigned sext)
{
    uint8_t buf[16] = { 0 };

    if (!emu_access(c, addr, buf, size, false)) {
        return false;
    }
    if (simd) {
        uint64_t *q = aa64_vfp_qreg(cpu_env(c->cpu), rt);

        q[0] = ldq_le_p(buf);
        q[1] = ldq_le_p(buf + 8);
        memset((uint8_t *)q + 16, 0,
               sizeof(cpu_env(c->cpu)->vfp.zregs[rt]) - 16);
    } else {
        uint64_t v = ldq_le_p(buf);

        if (sext) {
            v = sextract64(v, 0, size * 8);
            if (sext == 32) {
                v = (uint32_t)v;
            }
        }
        c->set_reg(c->cpu, rt, v);
    }
    return true;
}

/* Load/store register: immediate, unscaled, pre/post-indexed, register. */
static bool emu_ldst_single(EmuCtx *c, uint32_t inst)
{
    unsigned size = extract32(inst, 30, 2);
    bool simd = extract32(inst, 26, 1);
    unsigned opc = extract32(inst, 22, 2);
    int rn = extract32(inst, 5, 5);
    int rt = extract32(inst, 0, 5);
    unsigned scale, bytes, sext = 0;
    bool is_load, writeback = false, post = false;
    uint64_t base = emu_get_base(c, rn), addr;
    int64_t off;

    if (simd) {
        scale = (opc & 2) ? 4 : size;      /* opc<1> with size 00: 128-bit */
        if ((opc & 2) && size != 0) {
            return false;
        }
        is_load = opc & 1;
    } else {
        scale = size;
        if (opc == 0) {
            is_load = false;
        } else if (opc == 1) {
            is_load = true;
        } else if (size == 3) {
            return false;                   /* PRFM: nothing to emulate */
        } else {
            if (size == 2 && opc == 3) {
                return false;
            }
            is_load = true;
            sext = opc == 2 ? 64 : 32;
        }
    }
    bytes = 1u << scale;

    if (extract32(inst, 24, 1)) {           /* unsigned offset */
        off = (int64_t)extract32(inst, 10, 12) << scale;
        addr = base + off;
    } else if (extract32(inst, 21, 1)) {    /* register offset */
        int rm = extract32(inst, 16, 5);
        unsigned option = extract32(inst, 13, 3);
        bool s = extract32(inst, 12, 1);
        uint64_t m = rm == 31 ? 0 : c->get_reg(c->cpu, rm);

        if (extract32(inst, 10, 2) != 2) {
            return false;
        }
        switch (option) {
        case 2: m = (uint32_t)m; break;                 /* UXTW */
        case 3: break;                                  /* LSL/UXTX */
        case 6: m = (int64_t)(int32_t)m; break;         /* SXTW */
        case 7: break;                                  /* SXTX */
        default: return false;
        }
        addr = base + (m << (s ? scale : 0));
    } else {                                /* imm9: unscaled, post, pre */
        unsigned mode = extract32(inst, 10, 2);

        off = sextract32(inst, 12, 9);
        if (mode == 2) {
            return false;                   /* LDTR/STTR */
        }
        writeback = mode != 0;
        post = mode == 1;
        addr = post ? base : base + off;
        if (writeback) {
            base += off;
        }
    }

    if (is_load ? !emu_load(c, addr, rt, bytes, simd, sext)
                : !emu_store(c, addr, rt, bytes, simd)) {
        return false;
    }
    if (writeback) {
        emu_set_base(c, rn, base);
    }
    return true;
}

/* Load/store pair: signed offset, pre/post-indexed, non-temporal. */
static bool emu_ldst_pair(EmuCtx *c, uint32_t inst)
{
    unsigned opc = extract32(inst, 30, 2);
    bool simd = extract32(inst, 26, 1);
    unsigned mode = extract32(inst, 23, 2);
    bool is_load = extract32(inst, 22, 1);
    int rt2 = extract32(inst, 10, 5);
    int rn = extract32(inst, 5, 5);
    int rt = extract32(inst, 0, 5);
    unsigned scale, sext = 0;
    uint64_t base = emu_get_base(c, rn), addr;
    int64_t off;

    if (simd) {
        if (opc == 3) {
            return false;
        }
        scale = 2 + opc;
    } else if (opc == 0) {
        scale = 2;
    } else if (opc == 2) {
        scale = 3;
    } else if (opc == 1 && is_load) {
        scale = 2;                          /* LDPSW */
        sext = 64;
    } else {
        return false;                       /* STGP and reserved */
    }
    off = (int64_t)sextract32(inst, 15, 7) << scale;
    addr = mode == 1 ? base : base + off;   /* post-indexed: base as is */

    if (is_load) {
        if (!emu_load(c, addr, rt, 1u << scale, simd, sext) ||
            !emu_load(c, addr + (1u << scale), rt2, 1u << scale, simd, sext)) {
            return false;
        }
    } else if (!emu_store(c, addr, rt, 1u << scale, simd) ||
               !emu_store(c, addr + (1u << scale), rt2, 1u << scale, simd)) {
        return false;
    }
    if (mode == 1 || mode == 3) {
        emu_set_base(c, rn, base + off);
    }
    return true;
}

bool arm_aarch64_fallback_emu_single(CPUState *cpu, AddressSpace *as,
                                     ArmAarch64FallbackEmuGetReg get_reg,
                                     ArmAarch64FallbackEmuSetReg set_reg)
{
    EmuCtx c = { cpu, as, get_reg, set_reg };
    CPUARMState *env = cpu_env(cpu);
    uint32_t inst;
    hwaddr pc_pa;

    cpu_synchronize_state(cpu);

    pc_pa = arm_aarch64_fallback_emu_vtop(cpu, env->pc);
    if (pc_pa == -1 ||
        address_space_read(as, pc_pa, MEMTXATTRS_UNSPECIFIED, &inst,
                           sizeof(inst)) != MEMTX_OK) {
        fprintf(stderr, "%s: can't read the instruction at 0x%" PRIx64 "\n",
                __func__, env->pc);
        return false;
    }
    inst = le32_to_cpu(inst);

    if ((inst & 0x0a000000) == 0x08000000 && extract32(inst, 27, 3) == 5) {
        /* op0 x01, op1 0x101: load/store pair */
        if (emu_ldst_pair(&c, inst)) {
            return true;
        }
    } else if (extract32(inst, 27, 3) == 7 && extract32(inst, 25, 1) == 0) {
        /* x11 1x0x: load/store register */
        if (emu_ldst_single(&c, inst)) {
            return true;
        }
    }
    fprintf(stderr, "%s: can't emulate 0x%08x at 0x%" PRIx64 "\n", __func__,
            inst, env->pc);
    return false;
}
