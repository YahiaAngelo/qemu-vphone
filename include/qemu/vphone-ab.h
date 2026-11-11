/*
 * vphone-linux A/B switch for measurements in one running VM (accel/tcg/vphone-ab.c).
 * VPHONE_AB_FILE=<path>: a thread reads the file twice a second; when its first character
 * changes ('0' or '1'), vphone_ab_arm takes that value, every translation is flushed and
 * every vCPU's TLB with it, so code paths that test vphone_ab_arm at translation or run time
 * switch for the whole guest. VPHONE_AB=<0|1> sets the starting arm. Off (arm 0, no thread)
 * without VPHONE_AB_FILE. VPHONE_AB_WHAT=<feature> names the code path arm 1 switches
 * (vphone_ab()); a comma-separated list names several: asid_ng_off (cputlb.c: full flush on
 * an ASID switch, as before), tb_recycle (tb-maint.c), and the translator's ret_prefetch_off,
 * xpage_off, ras_off, ibc_off (target/arm/tcg/translate-a64.c: VPHONE_RET_PREFETCH,
 * VPHONE_XPAGE_LINKS, VPHONE_RAS, VPHONE_IBC off).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_VPHONE_AB_H
#define QEMU_VPHONE_AB_H

extern int vphone_ab_arm;
extern const char *vphone_ab_what;

/* Arm 1, and VPHONE_AB_WHAT names @feature: the code path under test is switched. */
static inline bool vphone_ab(const char *feature)
{
    size_t n;
    const char *w;

    if (!__builtin_expect(vphone_ab_arm, 0) || !vphone_ab_what) {
        return false;
    }
    n = strlen(feature);
    for (w = vphone_ab_what; *w; ) {
        size_t m = strcspn(w, ",");
        if (m == n && !memcmp(w, feature, n)) {
            return true;
        }
        w += m + (w[m] == ',');
    }
    return false;
}

#endif
