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
#include "exec/tb-flush.h"

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
/*
 * vphone-linux: generations per address space. An ASID switch flushes the
 * softmmu TLB, which has no ASID tag, but changes no mapping: a mapping
 * changes only with a TLB invalidation (or a permission change), each of
 * which starts a new epoch. So the jump cache keeps a generation per
 * address space (tcg_switch_jmp_cache()): switching back to an ASID makes
 * the entries filled under it current again when no epoch began since, as
 * a TLB tagged by ASID would keep them. Every other flush starts a new
 * epoch, which forgets every address space's generation
 * (tcg_stale_jmp_cache()). Generation 0 is never current: a flushed page's
 * entries are marked with it (cputlb.c tb_jmp_cache_clear_page()).
 */
#define TB_JMP_ASID_SLOTS 256

/*
 * vphone-linux: a generation per bucket too. A page's entries all lie in
 * one bucket of 2^TB_JMP_BUCKET_SHIFT entries (tb-hash.h, the bucket of
 * tb_jmp_cache_hash_page()), so a TLB flush of a page starts a new
 * generation of its bucket, one store, instead of marking the bucket's
 * entries; a range flush does so for each page, and starts a new epoch
 * only past one page a bucket. An entry is current when both its
 * generation and its bucket's are.
 */
#define TB_JMP_BUCKET_SHIFT 8
#define TB_JMP_BUCKETS (TB_JMP_CACHE_SIZE >> TB_JMP_BUCKET_SHIFT)

/*
 * vphone-linux: a return-address stack (VPHONE_RAS, translate-a64.c). A call
 * (BL, BLR, BLRAA, ...) pushes its return address, the TB its call site last
 * returned to (tb->vphone_ret_tb: the return address is the call's next
 * instruction, in the same page, so that TB is fixed by the calling TB's
 * physical code) and 'ras_stamp'. A return (RET, RETAA, RETAB) pops the top
 * entry and jumps straight to its TB when the target is the pushed address,
 * the stamp is unchanged and the TB is valid (not CF_INVALID) with the
 * returning TB's flags, cs_base and cflags; anything else takes
 * helper_vphone_ras_miss, which also fills the call site's TB. 'ras_stamp'
 * changes at every event that changes or might change a mapping of this CPU
 * (whatever starts a jump cache generation or clears a page, an address
 * space switch, an emptied cache), so an unchanged stamp means the return
 * address still maps to the page the call ran from. A stale or mismatched
 * entry (longjmp, a context switch, an exception) only misses: the stack is
 * a prediction, checked before use.
 */
#define VPHONE_RAS_BITS 5
#define VPHONE_RAS_SIZE (1 << VPHONE_RAS_BITS)

typedef struct VphoneRasEntry {
    vaddr pc;
    TranslationBlock *tb;
    TranslationBlock **slot;
    uint64_t stamp;
} VphoneRasEntry;

typedef struct CPUJumpCache {
    struct rcu_head rcu;
    uint32_t gen;
    /* the last generation handed out; the epoch; the current address space */
    uint32_t last_gen;
    uint32_t epoch;
    uint32_t asid;
    /*
     * vphone-linux: TB invalidations of position-independent TBs, counted by
     * the invalidating CPU (atomic), and the count this CPU last acted on.
     * Such a TB may sit in any entry, so instead of emptying every CPU's
     * cache, an invalidation bumps 'inval' and each CPU starts a new epoch
     * at its next lookup (tb_jmp_cache_check_inval()); an older entry is
     * used again only after tb_lookup() has checked its TB is not
     * CF_INVALID. Next to 'gen', on the line every lookup reads.
     */
    uint32_t inval;
    uint32_t inval_seen;
    /* vphone-linux: the return-address stack (above); top = last pushed */
    uint32_t ras_top;
    uint64_t ras_stamp;
    VphoneRasEntry ras[VPHONE_RAS_SIZE];
    /* address space (its key + 1, 0 when empty) -> generation, this epoch */
    struct {
        uint32_t key;
        uint32_t epoch;
        uint32_t gen;
    } asid_gen[TB_JMP_ASID_SLOTS];
    uint32_t bgen[TB_JMP_BUCKETS];
    struct {
        TranslationBlock *tb;
        vaddr pc;
        uint64_t cs_base;
        const void *tc_ptr;
        uint32_t flags;
        uint32_t cflags;
        uint32_t gen;
        uint32_t bgen;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

/* A mapping of @jc's CPU may have changed: no return-stack entry is good. */
static inline void tb_jmp_cache_ras_stale(CPUJumpCache *jc)
{
    qatomic_set(&jc->ras_stamp, jc->ras_stamp + 1);
}

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
    jc->array[h].bgen = jc->bgen[h >> TB_JMP_BUCKET_SHIFT];
    qatomic_set(&jc->array[h].tb, tb);
}

/* Whether entry @h of @jc is of the current generations (keys aside). */
static inline bool tb_jmp_cache_current(const CPUJumpCache *jc, uint32_t h)
{
    return jc->array[h].gen == jc->gen &&
           jc->array[h].bgen == jc->bgen[h >> TB_JMP_BUCKET_SHIFT];
}

/* Act on TB invalidations by other CPUs since the last lookup (see 'inval'). */
static inline void tb_jmp_cache_check_inval(CPUState *cpu, CPUJumpCache *jc)
{
    uint32_t n = qatomic_read(&jc->inval);

    if (unlikely(n != jc->inval_seen)) {
        /* pairs with the barrier in tb_jmp_cache_inval_tb() */
        smp_rmb();
        jc->inval_seen = n;
        tcg_stale_jmp_cache(cpu);
    }
}

/* Make entry @h of @jc current again, its TB checked against the mappings. */
static inline void tb_jmp_cache_renew(CPUJumpCache *jc, uint32_t h)
{
    jc->array[h].gen = jc->gen;
    jc->array[h].bgen = jc->bgen[h >> TB_JMP_BUCKET_SHIFT];
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
