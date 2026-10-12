/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

#define TB_JMP_CACHE_BITS 16
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 *
 * A TLB flush does not empty the cache: it starts a new generation
 * (tcg_stale_jmp_cache()). An entry from an older generation names a TB
 * that was right for its pc under the mappings of the time; tb_lookup()
 * accepts it again once it has checked that the pc still maps to the code
 * the TB was translated from. Guests that switch address space often
 * (every such switch flushes the TLB) would otherwise run with a cold
 * cache and take the hash table lookup for most indirect branches.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    uint32_t gen;
    struct {
        TranslationBlock *tb;
        vaddr pc;
        uint32_t gen;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
