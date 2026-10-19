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

#define TB_JMP_CACHE_BITS 17
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
/*
 * vphone-linux: an entry also holds the TB's lookup keys (cs_base, flags,
 * cflags) and its code pointer, so a hit reads only the entry's cache line:
 * an iOS guest makes tens of millions of indirect-branch lookups a second,
 * and the TB's own line was a second cache miss on each (perf: 16% of
 * tb_lookup on the compare, 37% on the entry load). The copies are written
 * with the entry by its own CPU; other CPUs only clear 'tb'.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    uint32_t gen;
    struct {
        TranslationBlock *tb;
        vaddr pc;
        uint64_t cs_base;
        const void *tc_ptr;
        uint32_t flags;
        uint32_t cflags;
        uint32_t gen;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

/* Fill entry @h of @jc with @tb for @pc, in generation @gen. */
static inline void tb_jmp_cache_fill(CPUJumpCache *jc, uint32_t h, vaddr pc,
                                     TranslationBlock *tb)
{
    jc->array[h].pc = pc;
    jc->array[h].cs_base = tb->cs_base;
    jc->array[h].tc_ptr = tb->tc.ptr;
    jc->array[h].flags = tb->flags;
    jc->array[h].cflags = tb->cflags;
    jc->array[h].gen = jc->gen;
    qatomic_set(&jc->array[h].tb, tb);
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
