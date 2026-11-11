/*
 * vphone-linux A/B switch for measurements in one running VM (accel/tcg/vphone-ab.c).
 * VPHONE_AB_FILE=<path>: a thread reads the file twice a second; when its first character
 * changes ('0' or '1'), vphone_ab_arm takes that value, every translation is flushed and
 * every vCPU's TLB with it, so code paths that test vphone_ab_arm at translation or run time
 * switch for the whole guest. VPHONE_AB=<0|1> sets the starting arm. Off (arm 0, no thread)
 * without VPHONE_AB_FILE. VPHONE_AB_WHAT=<feature> names the code path arm 1 switches
 * (vphone_ab()): asid_ng_off (cputlb.c: full flush on an ASID switch, as before).
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
    return __builtin_expect(vphone_ab_arm, 0) && vphone_ab_what &&
           !strcmp(vphone_ab_what, feature);
}

#endif
