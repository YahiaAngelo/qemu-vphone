/*
 * vphone diagnostic (VPHONE_EVENT_STATS=<seconds>): per-second rates of guest events that
 * leave translated code (exceptions by kind, GXF enter/exit, ERET), TLB fills and flushes,
 * translations, MMIO and code-page writes. Each thread counts into its own array; a logger
 * thread prints the sums to stderr every <seconds>, as per-second rates rounded down (an event
 * rarer than once per <seconds> shows as 0: use a longer period, or "info jit" for TB flushes).
 * Off by default: one predictable branch.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_VPHONE_EV_H
#define QEMU_VPHONE_EV_H

enum {
    VEV_SVC_EL0, VEV_SVC_OTHER, VEV_IRQ, VEV_FIQ, VEV_DABT_EL0, VEV_DABT_EL1,
    VEV_IABT, VEV_UDEF, VEV_GENTER, VEV_GXF_ABORT, VEV_EXC_OTHER,
    VEV_ERET, VEV_GEXIT,
    VEV_TLB_FILL, VEV_FLUSH_MMUIDX, VEV_FLUSH_MMUIDX_ALL, VEV_FLUSH_ASID,
    VEV_FLUSH_PAGE, VEV_FLUSH_PAGE_ALL, VEV_FLUSH_RANGE, VEV_FLUSH_RANGE_ALL,
    VEV_TB_GEN, VEV_TB_GEN_KCYC, VEV_NOTDIRTY, VEV_MMIO,
    VEV_RANGE_FULL_SIZE, VEV_RANGE_FULL_LARGE, VEV_PAGE_FULL_LARGE,
    VEV_FLUSHIDX_ALL, VEV_FLUSHIDX_ONE, VEV_FLUSHIDX_OTHER, VEV_RANGE_BY_LOG,
    VEV_ASID_RECENT8, VEV_ASID_KEY_RECENT8, VEV_SPRR_EL0_FLUSH, VEV_SPRR_EL1_FLUSH,
    VEV_GXF_ERET_FLUSH, VEV_TB_FIRST, VEV_TB_SAMEKEY, VEV_TB_VARIANT,
    VEV_TB_XPAGE, VEV_TB_SAMEKEY_XPAGE, VEV_TB_RACE, VEV_TB_ONEINSN, VEV_TB_INVAL, VEV_TB_INVAL_DMA, VEV_TB_FLUSH,
    /* TLB fills per (core mmu_idx & 31) * 2 + (address bit 63) */
    VEV_FILLX, VEV_FILLX_END = VEV_FILLX + 64,
    VEV_MAX = VEV_FILLX_END
};

extern int vphone_ev_on;
extern __thread unsigned long long *vphone_ev_tls;
void vphone_ev_register(void);

static inline void vphone_ev_add(int k, unsigned long long n)
{
    if (__builtin_expect(vphone_ev_on, 0)) {
        if (__builtin_expect(!vphone_ev_tls, 0)) {
            vphone_ev_register();
        }
        vphone_ev_tls[k] += n;
    }
}

static inline void vphone_ev(int k)
{
    vphone_ev_add(k, 1);
}

#endif
