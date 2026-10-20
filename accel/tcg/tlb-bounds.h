/*
 * softmmu size bounds
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef ACCEL_TCG_TLB_BOUNDS_H
#define ACCEL_TCG_TLB_BOUNDS_H

/*
 * vphone-linux: 10, not 6. An iOS guest flushes its TLBs constantly, and a
 * TLB shrunk to 64 entries refilled by table walks: at 1024 entries QEMU's
 * share of vCPU time while paging fell from 44.6% to 43.0%. At 4096 the
 * flushes' memsets and the larger tables made boot 13% slower.
 */
#define CPU_TLB_DYN_MIN_BITS 10
#define CPU_TLB_DYN_MAX_BITS (32 - TARGET_PAGE_BITS)
#define CPU_TLB_DYN_DEFAULT_BITS 8

#endif /* ACCEL_TCG_TLB_BOUNDS_H */
