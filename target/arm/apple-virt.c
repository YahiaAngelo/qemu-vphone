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

/*
 * Modelled on m1n1's SPTM emulation (src/hv/hv_sprr.c). Once SPRR is on, the
 * vCPU runs on shadows of the guest's page tables whose leaves carry the
 * SPRR-resolved permissions of the current world, the unguarded one (XNU) or
 * the guarded one (SPTM at GL1, TXM at GL0). Shadow tables are shared between
 * trees by guest table and permissions, and only exist once one of their
 * entries is valid; until then the entries that point at them are invalid,
 * which no walk caches, so no TLB maintenance is needed when they appear.
 * Guest table pages are write-protected at stage 2 and a store to one is
 * emulated, then the entries it wrote are resynced, so the shadows are current
 * by the next instruction and the guest's own TLB maintenance applies to them.
 * Every switch between sets of trees is followed by a TLB flush, which the
 * guest executes with its MMU off. GXF aborts aren't modelled: an unguarded
 * fetch from guarded-only code is an ordinary instruction abort.
 * Apple's PAC adds the kernel key to the keys above EL0 and enables them
 * regardless of SCTLR, so the vCPU holds the effective keys and enables of
 * the current EL, switched at lower-EL exception entries and exception
 * returns, while `env` holds the guest's own.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/memalign.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/reset.h"
#include "hw/arm/apple_virt_hvc.h"
#include "cpu.h"
#include "apple-virt.h"
#include "internals.h"
#include "syndrome.h"

#define VIRT_MAX_CPUS     64
#define PAC_SCTLR_ENABLES (SCTLR_EnIA | SCTLR_EnIB | SCTLR_EnDA | SCTLR_EnDB)

#define GXF_PAGE_SIZE  0x4000
#define GXF_PT_ENTRIES 2048
#define GXF_OA_MASK    0x0000FFFFFFFFC000ULL
#define GXF_TTBR_BADDR 0x0000FFFFFFFFFFFEULL
#define GXF_TTBR_CNP   (1ULL << 0)
#define GXF_NO_ACCESS  (~0ULL)
#define GXF_MDSCR_SS   (1ULL << 0)
#define GXF_POOL_AGED  256
#define GXF_POOL_SIZE  (1 * GiB)

#define PTE_VALID  (1ULL << 0)
#define PTE_TABLE  (1ULL << 1)
#define PTE_AP_EL0 (1ULL << 6)
#define PTE_AP_RO  (1ULL << 7)
#define PTE_PXN    (1ULL << 53)
#define PTE_UXN    (1ULL << 54)

#define SPRR_R 4
#define SPRR_W 2
#define SPRR_X 1

typedef struct
{
    uint64_t leaf[16];
} GXFPerms;

typedef struct GXFMirror
{
    hwaddr             ipa;
    const GXFPerms*    perms;
    uint8_t            level;
    uint32_t           nents;
    uint32_t           refs;
    hwaddr             shadow;
    uint64_t*          host;
    uint64_t*          shadow_host;
    uint64_t*          seen;
    struct GXFMirror** children;
    // `GXFParent`s.
    GArray* parents;
} GXFMirror;

typedef struct
{
    GXFMirror* mirror;
    uint32_t   index;
} GXFParent;

typedef struct
{
    GXFMirror* top;
    uint64_t   ttbr;
    uint32_t   va_bits;
    uint32_t   active;
    uint64_t   lru;
} GXFRoot;

typedef struct
{
    hwaddr     ipa;
    bool       protected;
    uint32_t   steppers;
    GPtrArray* mirrors;
} GXFPage;

// The trees a world last ran on, which it holds active, and what they were loaded for.
typedef struct
{
    bool            valid;
    bool            mmu;
    uint64_t        tcr;
    uint64_t        ttbr[2];
    const GXFPerms* perms[2];
    GXFRoot*        roots[2];
    uint64_t        hw_ttbr[2];
} GXFWorldCache;

typedef struct
{
    // The vCPU runs the patched images; others (like the SEP's) are left alone.
    bool          managed;
    bool          shadowed;
    bool          guarded;
    GXFWorldCache worlds[2];
    GXFRoot*      cur[2];
    uint64_t      guest_ttbr[2];
    uint64_t      hw_ttbr[2];
    // A TLB flush the vCPU is running, and where it resumes after it.
    bool     flushing;
    uint64_t resume_pc;
    uint32_t resume_pstate;
    uint64_t resume_sctlr;
    // `virt.evictions` as of the vCPU's last TLB flush.
    uint64_t flushed_evictions;
    // The EL whose PAC keys and enables the vCPU holds.
    bool    pac_loaded;
    uint8_t pac_el;
    // The guest's own state while `env` holds the vCPU's, between `apple_virt_sync_begin` and `apple_virt_sync_end`.
    bool     sync_guarded;
    uint64_t guest_keys[10];
    uint64_t guest_sctlr;
    // A store to a guest page table that is being single-stepped; it can straddle two pages.
    uint32_t nr_step_pages;
    hwaddr   step_pages[2];
    uint64_t step_mdscr;
    uint32_t step_pstate;
    // The machine was reset during a step, so debug exceptions still exit to the host.
    bool untrap_debug;
} AppleVirtCPU;

static struct
{
    const AppleVirtOps* ops;
    QemuMutex           lock;
    hwaddr              pool_base;
    hwaddr              pool_end;
    hwaddr              pool_cursor;
    uint8_t*            pool_host;
    // Freed shadow tables, reused oldest first once enough have aged, so that the guest's TLB maintenance for them has
    // likely happened and the host only backs about as many tables as were ever live at once.
    GQueue   pool_free;
    uint32_t pool_used;
    // Untracked pages that were write-protected, whose stores could still fault on another vCPU.
    GHashTable* unprotected;
    // Evicted trees, whose stale walks a vCPU must flush before it runs on new shadows.
    uint64_t     evictions;
    hwaddr       stub;
    hwaddr       empty;
    GPtrArray*   perms;
    GHashTable*  mirrors;
    GHashTable*  pages;
    GPtrArray*   roots;
    uint64_t     lru_clock;
    AppleVirtCPU cpus[VIRT_MAX_CPUS];
} virt;

/* clang-format off */
// SPRR permission nibble to R/W/X; [GL:2][EL:2], with the EL quirks of real hardware (and m1n1).
static const uint8_t gxf_el_rwx[16] = {
    0,             SPRR_R|SPRR_X, SPRR_R,        SPRR_R|SPRR_W,
    0,             SPRR_R|SPRR_X, SPRR_R,        0,
    0,             SPRR_X,        SPRR_R,        SPRR_R|SPRR_W,
    0,             SPRR_R|SPRR_X, SPRR_R,        SPRR_R|SPRR_W,
};
static const uint8_t gxf_gl_rwx[16] = {
    0,             0,             0,             0,
    SPRR_R|SPRR_X, SPRR_R|SPRR_X, SPRR_R|SPRR_X, SPRR_R|SPRR_X,
    SPRR_R,        SPRR_R,        SPRR_R,        SPRR_R,
    SPRR_R|SPRR_W, SPRR_R|SPRR_W, SPRR_R|SPRR_W, SPRR_R|SPRR_W,
};
/* clang-format on */

static const uint8_t gxf_lvl_off[4] = {47, 36, 25, 14};

bool apple_virt_m1_gl_names;
bool apple_virt_trap_sysreg;

static AppleVirtCPU* virt_cpu(CPUState* cs)
{
    assert_cmpint(cs->cpu_index, <, VIRT_MAX_CPUS);
    return &virt.cpus[cs->cpu_index];
}

static bool gxf_protect(hwaddr page, bool writable) { return virt.ops->protect(page, GXF_PAGE_SIZE, writable); }

static uint8_t* gxf_host_ptr(hwaddr pa, hwaddr size)
{
    MemoryRegion* mr;
    hwaddr        xlat;
    hwaddr        len = size;

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(&address_space_memory, pa, &xlat, &len, true, MEMTXATTRS_UNSPECIFIED);
    if (!memory_region_is_ram(mr) || memory_region_is_rom(mr) || len < size) { return NULL; }
    return (uint8_t*)memory_region_get_ram_ptr(mr) + xlat;
}

static void gxf_evict_root(void);
static bool gxf_live_translate(CPUState* cs, uint64_t va, hwaddr* pa);

static hwaddr gxf_pool_alloc(void)
{
    hwaddr pa;

    static long aged = -1;

    // vphone-linux debug: VR_POOL_AGED overrides how many freed tables wait before reuse.
    if (aged < 0) {
        const char* e = getenv("VR_POOL_AGED");

        aged = e ? strtol(e, NULL, 0) : GXF_POOL_AGED;
    }
    if (g_queue_get_length(&virt.pool_free) < (guint)aged && virt.pool_cursor + GXF_PAGE_SIZE <= virt.pool_end) {
        pa                = virt.pool_cursor;
        virt.pool_cursor += GXF_PAGE_SIZE;
    }
    else {
        while (g_queue_is_empty(&virt.pool_free)) { gxf_evict_root(); }
        pa = virt.pool_base + GPOINTER_TO_UINT(g_queue_pop_head(&virt.pool_free)) * GXF_PAGE_SIZE;
    }

    virt.pool_used += 1;
    memset(virt.pool_host + (pa - virt.pool_base), 0, GXF_PAGE_SIZE);
    return pa;
}

static void gxf_pool_release(hwaddr pa)
{
    g_queue_push_tail(&virt.pool_free, GUINT_TO_POINTER((pa - virt.pool_base) / GXF_PAGE_SIZE));
    virt.pool_used -= 1;
}

static guint gxf_mirror_hash(gconstpointer key)
{
    const GXFMirror* m = key;

    return g_int64_hash(&m->ipa) ^ g_direct_hash(m->perms) ^ (m->level << 24) ^ m->nents;
}

static gboolean gxf_mirror_equal(gconstpointer a, gconstpointer b)
{
    const GXFMirror* x = a;
    const GXFMirror* y = b;

    return x->ipa == y->ipa && x->perms == y->perms && x->level == y->level && x->nents == y->nents;
}

static GXFPage* gxf_page(hwaddr ipa)
{
    hwaddr page = ipa & ~(hwaddr)(GXF_PAGE_SIZE - 1);

    return g_hash_table_lookup(virt.pages, &page);
}

static void gxf_track(GXFMirror* m)
{
    GXFPage* p = gxf_page(m->ipa);

    if (p == NULL) {
        p          = g_new0(GXFPage, 1);
        p->ipa     = m->ipa & ~(hwaddr)(GXF_PAGE_SIZE - 1);
        p->mirrors = g_ptr_array_new();
        g_hash_table_insert(virt.pages, &p->ipa, p);
        g_hash_table_remove(virt.unprotected, GSIZE_TO_POINTER(p->ipa));
    }
    g_ptr_array_add(p->mirrors, m);
}

static void gxf_page_free(GXFPage* p)
{
    if (p->protected) {
        gxf_protect(p->ipa, true);
        g_hash_table_add(virt.unprotected, GSIZE_TO_POINTER(p->ipa));
    }
    g_ptr_array_free(p->mirrors, true);
    g_free(p);
}

static void gxf_page_put(GXFPage* p)
{
    if (p->mirrors->len != 0 || p->steppers != 0) { return; }
    g_hash_table_remove(virt.pages, &p->ipa);
    gxf_page_free(p);
}

static void gxf_untrack(GXFMirror* m)
{
    GXFPage* p = gxf_page(m->ipa);

    g_ptr_array_remove_fast(p->mirrors, m);
    gxf_page_put(p);
}

static void gxf_mirror_unparent(GXFMirror* child, GXFMirror* m, uint32_t i)
{
    for (guint j = 0; j < child->parents->len; j++) {
        GXFParent* p = &g_array_index(child->parents, GXFParent, j);

        if (p->mirror == m && p->index == i) {
            g_array_remove_index_fast(child->parents, j);
            return;
        }
    }
    assert_not_reached();
}

static void gxf_mirror_put(GXFMirror* m)
{
    if (--m->refs != 0) { return; }

    if (m->children != NULL) {
        for (uint32_t i = 0; i < m->nents; i++) {
            if (m->children[i] != NULL) {
                gxf_mirror_unparent(m->children[i], m, i);
                gxf_mirror_put(m->children[i]);
            }
        }
        g_free(m->children);
    }
    g_hash_table_remove(virt.mirrors, m);
    gxf_untrack(m);
    if (m->shadow_host != NULL) { gxf_pool_release(m->shadow); }
    g_array_free(m->parents, true);
    g_free(m->seen);
    g_free(m);
}

// SPRR ignores the hierarchical permission bits, so they are dropped.
static uint64_t gxf_mirror_table_desc(GXFMirror* m)
{ return m->shadow_host != NULL ? m->shadow | PTE_TABLE | PTE_VALID : 0; }

static void gxf_mirror_materialise(GXFMirror* m)
{
    g_autoptr(GArray) parents = NULL;

    m->shadow      = gxf_pool_alloc();
    m->shadow_host = (uint64_t*)(virt.pool_host + (m->shadow - virt.pool_base));
    if (m->children != NULL) {
        for (uint32_t i = 0; i < m->nents; i++) {
            if (m->children[i] != NULL) { m->shadow_host[i] = cpu_to_le64(gxf_mirror_table_desc(m->children[i])); }
        }
    }

    // Allocating shadow tables can evict trees, so the parents are held.
    parents = g_array_copy(m->parents);
    for (guint j = 0; j < parents->len; j++) { g_array_index(parents, GXFParent, j).mirror->refs += 1; }
    for (guint j = 0; j < parents->len; j++) {
        GXFParent* p = &g_array_index(parents, GXFParent, j);

        if (p->mirror->children[p->index] != m) { continue; }
        if (p->mirror->shadow_host == NULL) { gxf_mirror_materialise(p->mirror); }
        else {
            qatomic_set(&p->mirror->shadow_host[p->index], cpu_to_le64(gxf_mirror_table_desc(m)));
        }
    }
    for (guint j = 0; j < parents->len; j++) { gxf_mirror_put(g_array_index(parents, GXFParent, j).mirror); }
}

static GXFMirror* gxf_mirror_get(hwaddr ipa, const GXFPerms* perms, uint8_t level, uint32_t nents);

static uint64_t gxf_mirror_desc(GXFMirror* m, uint32_t i, uint64_t d)
{
    uint32_t index;
    uint64_t leaf;

    if (m->children != NULL && m->children[i] != NULL) {
        gxf_mirror_unparent(m->children[i], m, i);
        gxf_mirror_put(m->children[i]);
        m->children[i] = NULL;
    }

    if (!(d & PTE_VALID)) { return 0; }

    if (m->level < 3 && (d & PTE_TABLE)) {
        hwaddr    next   = d & GXF_OA_MASK;
        GXFParent parent = {.mirror = m, .index = i};

        if (gxf_host_ptr(next, GXF_PAGE_SIZE) == NULL) {
            warn_report("apple-virt: L%u[%u] points at non-RAM 0x%" HWADDR_PRIx ", leaving it unmapped", m->level, i,
                        next);
            return 0;
        }
        if (m->children == NULL) { m->children = g_new0(GXFMirror*, m->nents); }
        m->children[i] = gxf_mirror_get(next, m->perms, m->level + 1, GXF_PT_ENTRIES);
        g_array_append_val(m->children[i]->parents, parent);
        return gxf_mirror_table_desc(m->children[i]);
    }

    // A level 3 descriptor without the table bit is reserved.
    if (m->level == 3 && !(d & PTE_TABLE)) { return 0; }

    index = ((d & PTE_AP_RO) ? 8 : 0) | ((d & PTE_AP_EL0) ? 4 : 0) | ((d & PTE_UXN) ? 2 : 0) | ((d & PTE_PXN) ? 1 : 0);
    leaf  = m->perms->leaf[index];
    if (leaf == GXF_NO_ACCESS) { return 0; }
    return (d & ~(PTE_AP_RO | PTE_AP_EL0 | PTE_PXN | PTE_UXN)) | leaf;
}

// Mirrors entries that changed since the last sync; the page is protected first so no store is missed.
static void gxf_mirror_sync_range(GXFMirror* m, uint32_t first, uint32_t count)
{
    GXFPage* p = gxf_page(m->ipa);

    if (p != NULL && !p->protected && p->steppers == 0) {
        p->protected = gxf_protect(p->ipa, false);
        if (!p->protected) {
            warn_report("apple-virt: cannot write-protect guest page table at 0x%" HWADDR_PRIx, p->ipa);
        }
    }

    // Resyncing children can evict roots, so hold `m` for the duration.
    m->refs += 1;
    for (uint32_t i = first; i < first + count; i++) {
        uint64_t d = le64_to_cpu(qatomic_read(&m->host[i]));
        uint64_t shadow;

        if (d == m->seen[i]) { continue; }
        m->seen[i] = d;
        shadow     = gxf_mirror_desc(m, i, d);
        if (shadow != 0 && m->shadow_host == NULL) { gxf_mirror_materialise(m); }
        if (m->shadow_host != NULL) { qatomic_set(&m->shadow_host[i], cpu_to_le64(shadow)); }
    }
    gxf_mirror_put(m);
}

static void gxf_mirror_sync(GXFMirror* m) { gxf_mirror_sync_range(m, 0, m->nents); }

static GXFMirror* gxf_mirror_get(hwaddr ipa, const GXFPerms* perms, uint8_t level, uint32_t nents)
{
    GXFMirror  key = {.ipa = ipa, .perms = perms, .level = level, .nents = nents};
    GXFMirror* m   = g_hash_table_lookup(virt.mirrors, &key);

    if (m != NULL) {
        m->refs += 1;
        return m;
    }

    m          = g_new0(GXFMirror, 1);
    *m         = key;
    m->refs    = 1;
    m->host    = (uint64_t*)gxf_host_ptr(ipa, nents * sizeof(uint64_t));
    m->parents = g_array_new(false, false, sizeof(GXFParent));
    assert_nonnull(m->host);
    // All-ones sets RES0 bits, so no descriptor is ever that and the first sync mirrors every entry.
    m->seen = g_new(uint64_t, nents);
    memset(m->seen, 0xFF, nents * sizeof(uint64_t));
    g_hash_table_add(virt.mirrors, m);
    gxf_track(m);
    gxf_mirror_sync(m);
    return m;
}

static void gxf_root_free(GXFRoot* r)
{
    g_ptr_array_remove_fast(virt.roots, r);
    gxf_mirror_put(r->top);
    g_free(r);
}

static void gxf_evict_root(void)
{
    GXFRoot* victim = NULL;

    for (guint i = 0; i < virt.roots->len; i++) {
        GXFRoot* r = g_ptr_array_index(virt.roots, i);

        if (r->active == 0 && (victim == NULL || r->lru < victim->lru)) { victim = r; }
    }
    if (victim == NULL) {
        error_report("apple-virt: shadow page table pool exhausted by active trees (%u tables)", virt.pool_used);
        abort();
    }
    gxf_root_free(victim);
    qatomic_set(&virt.evictions, virt.evictions + 1);
}

static void gxf_resolve_perms(bool guarded, uint64_t perm_el0, uint64_t perm_el1, GXFPerms* out)
{
    const uint8_t* rwx = guarded ? gxf_gl_rwx : gxf_el_rwx;

    // What can't be expressed is over-granted, like m1n1.
    for (int index = 0; index < 16; index++) {
        uint8_t  k    = rwx[(perm_el1 >> (index * 4)) & 0xF];
        uint8_t  u    = rwx[(perm_el0 >> (index * 4)) & 0xF];
        uint64_t bits = 0;

        if (k == 0 && u == 0) {
            out->leaf[index] = GXF_NO_ACCESS;
            continue;
        }
        if (!(k & SPRR_X)) { bits |= PTE_PXN; }
        if (!(u & SPRR_X)) { bits |= PTE_UXN; }
        // PAN applies to what EL0 may read through the index; for an EL0 execute-only one, granting EL0 reads to
        // apply PAN like TCG would over-grant more.
        if (u & SPRR_R) { bits |= PTE_AP_EL0; }
        if (!((k | u) & SPRR_W)) { bits |= PTE_AP_RO; }
        out->leaf[index] = bits;
    }
}

static bool gxf_perms_match(const GXFPerms* p, bool guarded, uint64_t perm_el0, uint64_t perm_el1)
{
    GXFPerms want;

    gxf_resolve_perms(guarded, perm_el0, perm_el1, &want);
    return memcmp(p->leaf, want.leaf, sizeof(want.leaf)) == 0;
}

static const GXFPerms* gxf_get_perms(bool guarded, uint64_t perm_el0, uint64_t perm_el1)
{
    GXFPerms  want;
    GXFPerms* p;

    gxf_resolve_perms(guarded, perm_el0, perm_el1, &want);
    for (guint i = 0; i < virt.perms->len; i++) {
        p = g_ptr_array_index(virt.perms, i);
        if (memcmp(p->leaf, want.leaf, sizeof(want.leaf)) == 0) { return p; }
    }

    p = g_memdup2(&want, sizeof(want));
    g_ptr_array_add(virt.perms, p);
    return p;
}

static GXFRoot* gxf_get_root(uint64_t ttbr, uint32_t va_bits, const GXFPerms* perms)
{
    hwaddr   base = ttbr & GXF_TTBR_BADDR;
    GXFRoot* r;
    int      start;
    uint32_t nents;

    for (guint i = 0; i < virt.roots->len; i++) {
        r = g_ptr_array_index(virt.roots, i);
        if ((r->ttbr & GXF_TTBR_BADDR) == base && r->va_bits == va_bits && r->top->perms == perms) {
            r->ttbr = ttbr;
            r->lru  = ++virt.lru_clock;
            return r;
        }
    }

    for (start = 0; start < 3 && va_bits <= gxf_lvl_off[start]; start++) { }
    nents = 1U << (va_bits - gxf_lvl_off[start]);
    if (gxf_host_ptr(base, nents * sizeof(uint64_t)) == NULL) {
        warn_report("apple-virt: TTBR points at non-RAM 0x%" HWADDR_PRIx ", leaving it unmapped", base);
        return NULL;
    }

    r          = g_new0(GXFRoot, 1);
    r->ttbr    = ttbr;
    r->va_bits = va_bits;
    r->lru     = ++virt.lru_clock;
    // Pin it while it is built so that the pool can't evict it.
    r->active = 1;
    g_ptr_array_add(virt.roots, r);

    r->top = gxf_mirror_get(base, perms, start, nents);
    if (r->top->shadow_host == NULL) { gxf_mirror_materialise(r->top); }
    r->active = 0;
    return r;
}

static void gxf_world_drop(GXFWorldCache* wc)
{
    for (int idx = 0; idx < 2; idx++) {
        if (wc->roots[idx] != NULL) { wc->roots[idx]->active -= 1; }
    }
    *wc = (GXFWorldCache){0};
}

static void gxf_release(AppleVirtCPU* c)
{
    gxf_world_drop(&c->worlds[0]);
    gxf_world_drop(&c->worlds[1]);
    c->cur[0]   = NULL;
    c->cur[1]   = NULL;
    c->shadowed = false;
}

/*
 * vphone-linux: qemu-vphone models the M1-style SPRR layout that vresearch101's
 * SPTM uses (env->sprr_cfg.unified_perms): S3_6_C15_C1_5 is EL0's permission
 * table, S3_6_C15_C1_6 EL1's (for both translation regimes' halves), and
 * S3_6_C15_C3_0 EL0's write mask (m1n1's SPRR_UMPRR_EL1). Inferno's names for
 * those encodings (UPERM_BR0/BR1, PPERM_BR1) follow a later layout.
 */
static inline uint64_t virt_sprr_perm_el0(CPUARMState* env) { return env->sprr.sprr_el_br_el1[0][0]; }
static inline uint64_t virt_sprr_perm_el1(CPUARMState* env) { return env->sprr.sprr_el_br_el1[1][1]; }

// Keeps the copies qemu-vphone's TCG model also reads in step with the tables.
static void virt_sprr_mirror(CPUARMState* env)
{
    env->sprr.sprr_el_br_el1[0][1] = env->sprr.sprr_el_br_el1[0][0];
    env->sprr.sprr_el_br_el1[1][0] = env->sprr.sprr_el_br_el1[1][1];
}

// The vCPU state a world switch reads and its TLB flush changes, taken from `env` or straight from the vCPU.
typedef struct
{
    uint64_t pc;
    uint32_t pstate;
    uint64_t sctlr;
    uint64_t tcr;
    bool     guarded;
} GXFWorld;

static uint64_t gxf_env_ttbr(CPUARMState* env, int idx) { return idx ? env->cp15.ttbr1_el[1] : env->cp15.ttbr0_el[1]; }

// Whether the trees `wc` holds are those of the guest's current TTBRs, TCR and SPRR permissions.
static bool gxf_world_current(CPUARMState* env, const GXFWorldCache* wc, const GXFWorld* w)
{
    if (!wc->valid || wc->tcr != w->tcr || wc->mmu != !!(w->sctlr & SCTLR_M)) { return false; }
    for (int idx = 0; idx < 2; idx++) {
        if (wc->ttbr[idx] != gxf_env_ttbr(env, idx)
            || !gxf_perms_match(wc->perms[idx], w->guarded, virt_sprr_perm_el0(env), virt_sprr_perm_el1(env)))
        {
            return false;
        }
    }
    return true;
}

static void gxf_world_fill(CPUARMState* env, GXFWorldCache* wc, const GXFWorld* w)
{
    gxf_world_drop(wc);
    wc->valid = true;
    wc->mmu   = w->sctlr & SCTLR_M;
    wc->tcr   = w->tcr;

    for (int idx = 0; idx < 2; idx++) {
        uint64_t ttbr = gxf_env_ttbr(env, idx);
        uint32_t tsz  = extract64(w->tcr, idx ? 16 : 0, 6);
        bool     epd  = extract64(w->tcr, idx ? 23 : 7, 1);
        uint32_t tg   = extract64(w->tcr, idx ? 30 : 14, 2);

        wc->ttbr[idx]  = ttbr;
        wc->perms[idx] = gxf_get_perms(w->guarded, virt_sprr_perm_el0(env), virt_sprr_perm_el1(env));
        // The TCR may not be set up while the MMU is off; turning it on reloads the shadows.
        if (!epd && wc->mmu) {
            if (tg != (idx ? 1 : 2) || tsz < 16 || tsz > 50) {    // 16KiB granule
                error_report("apple-virt: unsupported TCR_EL1 0x%" PRIx64 " for shadow paging", w->tcr);
                abort();
            }
            wc->roots[idx] = gxf_get_root(ttbr, 64 - tsz, wc->perms[idx]);
            if (wc->roots[idx] != NULL) { wc->roots[idx]->active += 1; }
        }

        // The shadow shares the guest's ASID but has other permissions, so CnP must stay off.
        wc->hw_ttbr[idx] = (ttbr & ~(GXF_TTBR_BADDR | GXF_TTBR_CNP))
                           | (wc->roots[idx] != NULL ? wc->roots[idx]->top->shadow : virt.empty);
    }
}

static bool gxf_wants_shadows(CPUARMState* env) { return arm_is_sprr_enabled(env); }

static GXFWorld gxf_env_world(CPUARMState* env)
{
    return (GXFWorld){
        .pc      = env->pc,
        .pstate  = pstate_read(env),
        .sctlr   = env->cp15.sctlr_el[1],
        .tcr     = env->cp15.tcr_el[1],
        .guarded = arm_is_guarded(env),
    };
}

// Changes PSTATE in `env`, whose current SP is banked by EL and SPSel.
static void gxf_env_set_pstate(CPUARMState* env, uint32_t pstate)
{
    aarch64_save_sp(env, arm_current_el(env));
    pstate_write(env, pstate);
    aarch64_restore_sp(env, arm_current_el(env));
}

static void gxf_env_apply(CPUARMState* env, const GXFWorld* w)
{
    env->pc               = w->pc;
    env->cp15.sctlr_el[1] = w->sctlr;
    if (w->pstate != pstate_read(env)) { gxf_env_set_pstate(env, w->pstate); }
}

// Runs `tlbi vmalle1[is]` in the guest at EL1, with its MMU off so no stale translation is used to fetch it.
static void gxf_flush_tlb(AppleVirtCPU* c, GXFWorld* w, bool broadcast)
{
    c->flushing          = true;
    c->resume_pc         = w->pc;
    c->resume_pstate     = w->pstate;
    c->resume_sctlr      = w->sctlr;
    c->flushed_evictions = qatomic_read(&virt.evictions);

    w->sctlr &= ~SCTLR_M;
    if (extract32(w->pstate, 2, 2) == 0) { w->pstate = deposit32(w->pstate, 0, 4, PSTATE_MODE_EL1h); }
    w->pstate |= PSTATE_DAIF;
    w->pc      = virt.stub + (broadcast ? 0x18 : 0);
}

static void gxf_update_world_regs(CPUARMState* env, AppleVirtCPU* c, GXFWorld* w, bool force_flush)
{
    GXFWorldCache* wc   = &c->worlds[w->guarded];
    bool           want = gxf_wants_shadows(env);
    bool           flush;

    flush =
        want != c->shadowed
        || (want && (force_flush || w->guarded != c->guarded || c->flushed_evictions != qatomic_read(&virt.evictions)));
    if (want) {
        if (!gxf_world_current(env, wc, w)) {
            QEMU_LOCK_GUARD(&virt.lock);
            gxf_world_fill(env, wc, w);
        }
        for (int idx = 0; idx < 2; idx++) {
            c->cur[idx]     = wc->roots[idx];
            c->hw_ttbr[idx] = wc->hw_ttbr[idx];
        }
    }
    else if (c->shadowed) {
        QEMU_LOCK_GUARD(&virt.lock);
        gxf_release(c);
    }

    c->shadowed = want;
    c->guarded  = w->guarded;
    if (flush) { gxf_flush_tlb(c, w, false); }
}

static void gxf_update_world(CPUARMState* env, AppleVirtCPU* c, bool force_flush)
{
    GXFWorld w = gxf_env_world(env);

    gxf_update_world_regs(env, c, &w, force_flush);
    gxf_env_apply(env, &w);
}

// Drops the idle trees of an address space the guest is done with, as their tables may be reused.
static void gxf_drop_asid(uint16_t asid)
{
    if (asid == 0) { return; }

    for (guint i = 0; i < virt.roots->len;) {
        GXFRoot* r = g_ptr_array_index(virt.roots, i);

        if (r->active == 0 && (r->ttbr >> 48) == asid) {
            gxf_root_free(r);
            continue;
        }
        i++;
    }
}

// The PAC key registers, in `APPLE_VIRT_VREG_APIAKEYLO_EL1` order.
static uint64_t* pac_key_field(CPUARMState* env, int k)
{
    ARMPACKey* keys[] = {&env->keys.apia, &env->keys.apib, &env->keys.apda, &env->keys.apdb, &env->keys.apga};

    return k & 1 ? &keys[k / 2]->hi : &keys[k / 2]->lo;
}

// What a write of `value` to `vreg` stores, as `pauth_write_lo` and `apctl_write` do it.
static uint64_t virt_vreg_write_value(CPUARMState* env, int vreg, uint64_t value)
{
    switch (vreg) {
        // In Apple mode, the keys are masked with the machine key.
        case APPLE_VIRT_VREG_APIAKEYLO_EL1 ... APPLE_VIRT_VREG_APGAKEYHI_EL1:
            if (!(env->cp15.apctl_el1 & APCTL_AppleMode)) { return value; }
            return value ^ ((vreg - APPLE_VIRT_VREG_APIAKEYLO_EL1) & 1 ? env->keys.m.hi : env->keys.m.lo);
        case APPLE_VIRT_VREG_APCTL_EL1: return (value & ~APCTL_MKEYVld) | (env->cp15.apctl_el1 & APCTL_MKEYVld);
        // VMSA_LOCK_EL1 applies to the unguarded world, as `sctlr_el1_write` does it.
        case APPLE_VIRT_VREG_SCTLR_EL1:
            if (arm_is_guarded(env)) { return value; }
            if (env->cp15.vmsa_lock_el1 & VMSA_LOCK_SCTLR_EL1) { return env->cp15.sctlr_el[1]; }
            if (env->cp15.vmsa_lock_el1 & VMSA_LOCK_SCTLR_M_BIT) {
                return (value & ~SCTLR_M) | (env->cp15.sctlr_el[1] & SCTLR_M);
            }
            return value;
        default: return value;
    }
}

// Apple's PAC uses the kernel key on top of the key above EL0, as in the TCG model.
static uint64_t pac_key(CPUARMState* env, int k, int el)
{
    uint64_t key = *pac_key_field(env, k);

    if (el > 0 && (env->cp15.apctl_el1 & APCTL_KernKeyEn)) { key ^= k & 1 ? env->keys.kernel.hi : env->keys.kernel.lo; }
    return key;
}

// What the vCPU's SCTLR holds for the guest's `sctlr` at `el`: Apple's keys are always on above EL0, while the
// host's follow SCTLR.
static uint64_t pac_hw_sctlr(CPUARMState* env, uint64_t sctlr, int el)
{
    if (el > 0 && (env->cp15.apctl_el1 & APCTL_AppleMode)) { return sctlr | PAC_SCTLR_ENABLES; }
    return sctlr;
}

static void pac_install(CPUState* cs, CPUARMState* env, AppleVirtCPU* c, int el)
{
    for (int k = 0; k < ARRAY_SIZE(c->guest_keys); k++) {
        virt.ops->set_reg(cs, APPLE_VIRT_REG_APIAKEYLO_EL1 + k, pac_key(env, k, el));
    }
    virt.ops->set_reg(cs, APPLE_VIRT_REG_SCTLR_EL1, pac_hw_sctlr(env, env->cp15.sctlr_el[1], el));
    c->pac_loaded = true;
    c->pac_el     = el;
}

static void pac_load(CPUState* cs, CPUARMState* env, AppleVirtCPU* c, int el)
{
    if (!c->pac_loaded || c->pac_el != el) { pac_install(cs, env, c, el); }
}

// From GL1 the `_GL11` registers are the unguarded world's, as in the TCG model.
static uint64_t* virt_vreg_field(CPUARMState* env, int vreg)
{
    switch (vreg) {
        case APPLE_VIRT_VREG_SPRR_CONFIG_EL1   : return &env->sprr.sprr_config_el[1];
        case APPLE_VIRT_VREG_GXF_CONFIG_EL1    : return &env->gxf.gxf_config_el[1];
        case APPLE_VIRT_VREG_GXF_STATUS_EL1    : return &env->gxf.gxf_status_el[1];
        case APPLE_VIRT_VREG_GXF_ENTER_EL1     : return &env->gxf.gxf_enter_el[1];
        case APPLE_VIRT_VREG_GXF_ABORT_EL1     : return &env->gxf.gxf_abort_el[1];
        case APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0: return &env->sprr.sprr_el_br_el1[0][0];    // C1_5: EL0 table
        case APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0: return &env->sprr.sprr_el_br_el1[1][1];    // C1_6: EL1 table
        case APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1: return &env->sprr.c1_7;                    // C1_7: unused
        case APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1: return &env->sprr.c3_0;                    // C3_0: EL0 mask
        case APPLE_VIRT_VREG_ASPSR_GL11        : return &env->gxf.aspsr_gl[1];
        case APPLE_VIRT_VREG_SP_GL11           : return &env->sp_el[1];
        case APPLE_VIRT_VREG_TPIDR_GL11        : return &env->cp15.tpidr_el[1];
        case APPLE_VIRT_VREG_VBAR_GL11         : return &env->cp15.vbar_el[1];
        case APPLE_VIRT_VREG_SPSR_GL11         : return &env->banked_spsr[BANK_SVC];
        case APPLE_VIRT_VREG_ESR_GL11          : return &env->cp15.esr_el[1];
        case APPLE_VIRT_VREG_ELR_GL11          : return &env->elr_el[1];
        case APPLE_VIRT_VREG_FAR_GL11          : return &env->cp15.far_el[1];
        case APPLE_VIRT_VREG_TTBR0_EL1         : return &env->cp15.ttbr0_el[1];
        case APPLE_VIRT_VREG_TTBR1_EL1         : return &env->cp15.ttbr1_el[1];
        case APPLE_VIRT_VREG_TCR_EL1           : return &env->cp15.tcr_el[1];
        case APPLE_VIRT_VREG_SCTLR_EL1         : return &env->cp15.sctlr_el[1];
        case APPLE_VIRT_VREG_SP_EL1            : return &env->sp_el[1];
        case APPLE_VIRT_VREG_APIAKEYLO_EL1 ... APPLE_VIRT_VREG_APGAKEYHI_EL1:
            return pac_key_field(env, vreg - APPLE_VIRT_VREG_APIAKEYLO_EL1);
        case APPLE_VIRT_VREG_APCTL_EL1    : return &env->cp15.apctl_el1;
        case APPLE_VIRT_VREG_KERNKEYLO_EL1: return &env->keys.kernel.lo;
        case APPLE_VIRT_VREG_KERNKEYHI_EL1: return &env->keys.kernel.hi;
        // With the state synced, `env` holds each world's registers in its own fields.
        case APPLE_VIRT_VREG_M1_TPIDR_EL1 : return &env->cp15.tpidr_el[1];
        case APPLE_VIRT_VREG_M1_VBAR_EL1  : return &env->cp15.vbar_el[1];
        case APPLE_VIRT_VREG_M1_SPSR_EL1  : return &env->banked_spsr[BANK_SVC];
        case APPLE_VIRT_VREG_M1_ELR_EL1   : return &env->elr_el[1];
        case APPLE_VIRT_VREG_M1_ESR_EL1   : return &env->cp15.esr_el[1];
        case APPLE_VIRT_VREG_M1_FAR_EL1   : return &env->cp15.far_el[1];
        case APPLE_VIRT_VREG_M1_SP_GL1    : return &env->gxf.sp_gl[1];
        case APPLE_VIRT_VREG_M1_TPIDR_GL1 : return &env->gxf.tpidr_gl[1];
        case APPLE_VIRT_VREG_M1_VBAR_GL1  : return &env->gxf.vbar_gl[1];
        case APPLE_VIRT_VREG_M1_SPSR_GL1  : return &env->gxf.spsr_gl[1];
        case APPLE_VIRT_VREG_M1_ASPSR_GL1 : return &env->gxf.aspsr_gl[1];
        case APPLE_VIRT_VREG_M1_ESR_GL1   : return &env->gxf.esr_gl[1];
        case APPLE_VIRT_VREG_M1_ELR_GL1   : return &env->gxf.elr_gl[1];
        case APPLE_VIRT_VREG_M1_FAR_GL1   : return &env->gxf.far_gl[1];
        default                           : assert_not_reached();
    }
}

static bool virt_handle_sysreg(CPUARMState* env, AppleVirtCPU* c, uint16_t imm)
{
    int       vreg = APPLE_VIRT_HVC_VREG(imm);
    uint32_t  rt   = APPLE_VIRT_HVC_RT(imm);
    uint64_t* field;
    uint64_t  value;

    if (vreg >= APPLE_VIRT_VREG_MAX) { return false; }
    // GXF_STATUS is read-only, and ASPSR and the `_GL11` registers are only accessible while guarded.
    if (vreg == APPLE_VIRT_VREG_GXF_STATUS_EL1 && !APPLE_VIRT_HVC_READ(imm)) { return false; }
    if (vreg >= APPLE_VIRT_VREG_ASPSR_GL11 && vreg <= APPLE_VIRT_VREG_FAR_GL11 && !arm_is_guarded(env)) {
        return false;
    }

    // SP_EL1 is the current world's, which is the current SP at EL1h and GL1h.
    if (vreg == APPLE_VIRT_VREG_SP_EL1 && (env->pstate & PSTATE_SP)) { field = &env->xregs[31]; }
    else if (vreg == APPLE_VIRT_VREG_SP_EL1 && arm_is_guarded(env)) {
        field = &env->gxf.sp_gl[1];
    }
    else {
        field = virt_vreg_field(env, vreg);
    }

    if (APPLE_VIRT_HVC_READ(imm)) {
        if (rt != 31) { env->xregs[rt] = *field; }
        return true;
    }

    value = virt_vreg_write_value(env, vreg, (rt == 31) ? 0 : env->xregs[rt]);
    if (value == *field) { return true; }
    *field = value;
    virt_sprr_mirror(env);

    switch (vreg) {
        case APPLE_VIRT_VREG_TTBR0_EL1:
        case APPLE_VIRT_VREG_TTBR1_EL1:
        case APPLE_VIRT_VREG_TCR_EL1  :
        case APPLE_VIRT_VREG_SCTLR_EL1:
            // The guest does its own TLB maintenance for a new translation.
            gxf_update_world(env, c, false);
            break;
        case APPLE_VIRT_VREG_SPRR_CONFIG_EL1   :
        case APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0:
        case APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0:
        case APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1:
        case APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1:
            // The shadows bake the permissions into the TLB entries.
            gxf_update_world(env, c, true);
            break;
        default: break;
    }
    return true;
}

// The registers genter/gexit bank, in the vCPU while their world runs and in `env` otherwise.
static const AppleVirtReg gxf_bank_regs[] = {
    APPLE_VIRT_REG_SP_EL1,  APPLE_VIRT_REG_TPIDR_EL1, APPLE_VIRT_REG_VBAR_EL1, APPLE_VIRT_REG_SPSR_EL1,
    APPLE_VIRT_REG_ELR_EL1, APPLE_VIRT_REG_ESR_EL1,   APPLE_VIRT_REG_FAR_EL1,
};

static uint64_t* gxf_bank_field(CPUARMState* env, bool guarded, int i)
{
    switch (i) {
        case 0 : return guarded ? &env->gxf.sp_gl[1] : &env->sp_el[1];
        case 1 : return guarded ? &env->gxf.tpidr_gl[1] : &env->cp15.tpidr_el[1];
        case 2 : return guarded ? &env->gxf.vbar_gl[1] : &env->cp15.vbar_el[1];
        case 3 : return guarded ? &env->gxf.spsr_gl[1] : &env->banked_spsr[BANK_SVC];
        case 4 : return guarded ? &env->gxf.elr_gl[1] : &env->elr_el[1];
        case 5 : return guarded ? &env->gxf.esr_gl[1] : &env->cp15.esr_el[1];
        case 6 : return guarded ? &env->gxf.far_gl[1] : &env->cp15.far_el[1];
        default: assert_not_reached();
    }
}

static void gxf_swap_bank(CPUState* cs, CPUARMState* env, bool from)
{
    for (int i = 0; i < ARRAY_SIZE(gxf_bank_regs); i++) {
        *gxf_bank_field(env, from, i) = virt.ops->get_reg(cs, gxf_bank_regs[i]);
        virt.ops->set_reg(cs, gxf_bank_regs[i], *gxf_bank_field(env, !from, i));
    }
}

static void gxf_switch_world_fast(CPUState* cs, CPUARMState* env, AppleVirtCPU* c, GXFWorld* w, bool force_flush)
{
    gxf_update_world_regs(env, c, w, force_flush);
    virt.ops->set_reg(cs, APPLE_VIRT_REG_TTBR0_EL1, c->shadowed ? c->hw_ttbr[0] : env->cp15.ttbr0_el[1]);
    virt.ops->set_reg(cs, APPLE_VIRT_REG_TTBR1_EL1, c->shadowed ? c->hw_ttbr[1] : env->cp15.ttbr1_el[1]);
    pac_load(cs, env, c, extract32(w->pstate, 2, 2));
    virt.ops->set_reg(cs, APPLE_VIRT_REG_SCTLR_EL1, pac_hw_sctlr(env, w->sctlr, extract32(w->pstate, 2, 2)));
    virt.ops->set_reg(cs, APPLE_VIRT_REG_CPSR, w->pstate);
    virt.ops->set_reg(cs, APPLE_VIRT_REG_PC, w->pc);
}

// Reloads the shadows on the vCPU itself for the GXF state, TTBRs, TCR, SCTLR or SPRR permissions in `env`, resuming
// after the instruction that changed them.
static void gxf_reload_world_fast(CPUState* cs, CPUARMState* env, AppleVirtCPU* c, bool sprr)
{
    GXFWorld w = {
        .pc      = virt.ops->get_reg(cs, APPLE_VIRT_REG_PC),
        .pstate  = virt.ops->get_reg(cs, APPLE_VIRT_REG_CPSR),
        .sctlr   = env->cp15.sctlr_el[1],
        .tcr     = env->cp15.tcr_el[1],
        .guarded = env->gxf.gxf_status_el[1] & 1,
    };

    // The shadows bake the permissions into the TLB entries, which stay valid if the leaves are the same.
    gxf_switch_world_fast(cs, env, c, &w, sprr && c->shadowed && !gxf_world_current(env, &c->worlds[w.guarded], &w));
}

static bool virt_eret_fast(CPUState* cs, CPUARMState* env, AppleVirtCPU* c)
{
    uint64_t spsr = virt.ops->get_reg(cs, APPLE_VIRT_REG_SPSR_EL1);
    int      el   = extract64(spsr, 2, 2);

    if (el > 1) { return false; }
    virt.ops->set_reg(cs, APPLE_VIRT_REG_CPSR, spsr & aarch64_pstate_valid_mask(&env_archcpu(env)->isar) & ~PSTATE_SS);
    virt.ops->set_reg(cs, APPLE_VIRT_REG_PC, virt.ops->get_reg(cs, APPLE_VIRT_REG_ELR_EL1));
    pac_load(cs, env, c, el);
    return true;
}

// EL0 reads of an SPRR register this side emulated (`virt_el0_sprr_fast`), by instruction address:
// the kernel answers them there itself. They are the shared cache's, at one address in every
// process. Appended only, the count after the entry.
static struct {
    uint64_t va[32];
    uint16_t imm[32];
    uint8_t  rt[32];
    uint32_t n;
    QemuMutex lock;
} apple_virt_el0_sites;

static void apple_virt_el0_site(uint64_t va, int vreg, uint32_t rt)
{
    uint16_t imm = APPLE_VIRT_HVC_SYSREG | (vreg << 6) | (1 << 5);
    uint32_t n   = qatomic_read(&apple_virt_el0_sites.n);

    for (uint32_t i = 0; i < n; i++) {
        if (apple_virt_el0_sites.va[i] == va) { return; }
    }
    if (n >= ARRAY_SIZE(apple_virt_el0_sites.va)) { return; }
    apple_virt_el0_sites.va[n]  = va;
    apple_virt_el0_sites.imm[n] = imm;
    apple_virt_el0_sites.rt[n]  = rt;
    qatomic_store_release(&apple_virt_el0_sites.n, n + 1);
}

// An EL0 SPRR access, which the vCPU UNDEFs to EL1, emulated as `sprr_uperm_br0_write` does it.
static bool virt_el0_sprr_fast(CPUState* cs, CPUARMState* env, AppleVirtCPU* c)
{
    uint64_t elr;
    uint64_t spsr;
    uint64_t mask = 0;
    uint64_t value;
    hwaddr   pa;
    uint32_t insn;
    uint32_t rt;
    bool     read;
    int      vreg;

    if (syn_get_ec(virt.ops->get_reg(cs, APPLE_VIRT_REG_ESR_EL1)) != EC_UNCATEGORIZED || (env->gxf.gxf_status_el[1] & 1)
        || cs->singlestep_enabled)
    {
        return false;
    }
    // The synchronous exception slot of a lower AArch64 EL, after the `hvc`.
    {
        uint64_t off = virt.ops->get_reg(cs, APPLE_VIRT_REG_PC) - virt.ops->get_reg(cs, APPLE_VIRT_REG_VBAR_EL1);

        // vphone-linux debug: 0x408 with VR_VECTOR_ISB's `isb` in front of the `hvc`.
        if (off != 0x404 && off != 0x408) { return false; }
    }
    spsr = virt.ops->get_reg(cs, APPLE_VIRT_REG_SPSR_EL1);
    elr  = virt.ops->get_reg(cs, APPLE_VIRT_REG_ELR_EL1);
    if ((spsr & (PSTATE_nRW | PSTATE_M)) != PSTATE_MODE_EL0t || !gxf_live_translate(cs, elr, &pa)) { return false; }

    insn = address_space_ldl_le(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, NULL);
    if ((insn & 0xFFD00000) != 0xD5100000) { return false; }
    vreg = apple_virt_vreg(2 + extract32(insn, 19, 1), extract32(insn, 16, 3), extract32(insn, 12, 4),
                           extract32(insn, 8, 4), extract32(insn, 5, 3));
    read = extract32(insn, 21, 1);
    rt   = extract32(insn, 0, 5);
    if (vreg != APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0 && !(vreg == APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0 && read)) {
        return false;
    }

    virt.ops->set_reg(cs, APPLE_VIRT_REG_ELR_EL1, elr + 4);
    if (read) {
        if (rt != 31) { virt.ops->set_xreg(cs, rt, *virt_vreg_field(env, vreg)); }
        // The kernel answers the next ones at this address (`apple_virt_fast_publish`).
        qemu_mutex_lock(&apple_virt_el0_sites.lock);
        apple_virt_el0_site(elr, vreg, rt);
        qemu_mutex_unlock(&apple_virt_el0_sites.lock);
    }
    else {
        // EL0 only changes the EL bits that SPRR_UMPRR_BR0_EL1 lets it.
        for (int i = 0; i < 16; i++) { mask |= extract64(env->sprr.c3_0, i * 2, 2) << (i * 4); }
        value = (virt_sprr_perm_el0(env) & ~mask) | ((rt == 31 ? 0 : virt.ops->get_xreg(cs, rt)) & mask);
        if (value != virt_sprr_perm_el0(env)) {
            env->sprr.sprr_el_br_el1[0][0] = value;
            virt_sprr_mirror(env);
            if (c->shadowed && c->cur[0] != NULL
                && !gxf_perms_match(c->cur[0]->top->perms, false, value, virt_sprr_perm_el1(env)))
            {
                GXFWorld w = {
                    .pc      = elr + 4,
                    .pstate  = spsr & aarch64_pstate_valid_mask(&env_archcpu(env)->isar) & ~PSTATE_SS,
                    .sctlr   = env->cp15.sctlr_el[1],
                    .tcr     = env->cp15.tcr_el[1],
                    .guarded = false,
                };
                gxf_switch_world_fast(cs, env, c, &w, true);
                return true;
            }
        }
    }
    virt_eret_fast(cs, env, c);
    return true;
}

// genter from EL1 to GL1, as `arm_cpu_do_interrupt` does it for EXCP_GENTER.
static bool gxf_genter_fast(CPUState* cs, CPUARMState* env, AppleVirtCPU* c, uint16_t imm)
{
    uint64_t pc;
    uint64_t cpsr;
    GXFWorld w;

    if (env->gxf.gxf_status_el[1] & 1) { return false; }

    cpsr = virt.ops->get_reg(cs, APPLE_VIRT_REG_CPSR);
    if (extract64(cpsr, 2, 2) != 1) { return false; }
    pc      = virt.ops->get_reg(cs, APPLE_VIRT_REG_PC);
    w.sctlr = env->cp15.sctlr_el[1];
    w.tcr   = env->cp15.tcr_el[1];

    env->gxf.spsr_gl[1]        = cpsr;
    env->gxf.elr_gl[1]         = pc;
    env->gxf.esr_gl[1]         = syn_aa64_genter(imm & ~APPLE_VIRT_HVC_GXF_MASK);
    env->gxf.aspsr_gl[1]      &= ~1ULL;
    env->gxf.gxf_status_el[1] |= 1;
    gxf_swap_bank(cs, env, false);

    w.pc      = env->gxf.gxf_enter_el[1];
    w.pstate  = PSTATE_DAIF | PSTATE_MODE_EL1h | (cpsr & PSTATE_PAN) | (w.sctlr & SCTLR_SPAN ? 0 : PSTATE_PAN);
    w.guarded = true;
    if (cpu_isar_feature(aa64_ssbs, env_archcpu(env)) && (w.sctlr & SCTLR_DSSBS_64)) { w.pstate |= PSTATE_SSBS; }

    gxf_switch_world_fast(cs, env, c, &w, false);
    return true;
}

// gexit from GL1 back to the unguarded world, as `aarch64_gxf_exit` does it.
static bool gxf_gexit_fast(CPUState* cs, CPUARMState* env, AppleVirtCPU* c)
{
    GXFWorld w;

    if (!(env->gxf.gxf_status_el[1] & 1) || (env->gxf.aspsr_gl[1] & 1)) { return false; }

    w.sctlr = env->cp15.sctlr_el[1];
    w.tcr   = env->cp15.tcr_el[1];
    gxf_swap_bank(cs, env, true);
    env->gxf.gxf_status_el[1] &= ~1ULL;

    w.pc      = env->gxf.elr_gl[1];
    w.pstate  = env->gxf.spsr_gl[1] & aarch64_pstate_valid_mask(&env_archcpu(env)->isar) & ~PSTATE_SS;
    w.guarded = false;

    gxf_switch_world_fast(cs, env, c, &w, false);
    return true;
}

static void gxf_resume_fast(CPUState* cs, CPUARMState* env, AppleVirtCPU* c)
{
    int el = extract32(c->resume_pstate, 2, 2);

    // A flush started with the state synced left the stub's SCTLR in `env`.
    env->cp15.sctlr_el[1] = c->resume_sctlr;
    c->flushing           = false;
    virt.ops->set_reg(cs, APPLE_VIRT_REG_SCTLR_EL1, pac_hw_sctlr(env, c->resume_sctlr, el));
    pac_load(cs, env, c, el);
    virt.ops->set_reg(cs, APPLE_VIRT_REG_CPSR, c->resume_pstate);
    virt.ops->set_reg(cs, APPLE_VIRT_REG_PC, c->resume_pc);
}

// An `msr`/`mrs` of a register that is only in `env`, or that the vCPU holds as it is.
// `after` is whether the vCPU is already after the instruction, which changes to the shadows need.
/*
 * vphone-linux: an M1-style monitor's access to a register genter/gexit bank (TPIDR, VBAR, SPSR,
 * ELR, ESR, FAR): its own (`_GL1`) or the unguarded world's (`_EL1`). The world that runs has its
 * bank in the vCPU's EL1 registers, the other's is in `env`.
 */
static bool virt_m1_bank_fast(CPUState* cs, CPUARMState* env, int vreg, bool read, uint32_t rt)
{
    bool      gl      = vreg >= APPLE_VIRT_VREG_M1_SP_GL1;
    int       i;
    bool      live    = gl == !!(env->gxf.gxf_status_el[1] & 1);
    uint64_t* field;
    uint64_t  value;

    // The index of gxf_bank_regs / gxf_bank_field.
    switch (vreg) {
        case APPLE_VIRT_VREG_M1_TPIDR_EL1: case APPLE_VIRT_VREG_M1_TPIDR_GL1: i = 1; break;
        case APPLE_VIRT_VREG_M1_VBAR_EL1 : case APPLE_VIRT_VREG_M1_VBAR_GL1 : i = 2; break;
        case APPLE_VIRT_VREG_M1_SPSR_EL1 : case APPLE_VIRT_VREG_M1_SPSR_GL1 : i = 3; break;
        case APPLE_VIRT_VREG_M1_ELR_EL1  : case APPLE_VIRT_VREG_M1_ELR_GL1  : i = 4; break;
        case APPLE_VIRT_VREG_M1_ESR_EL1  : case APPLE_VIRT_VREG_M1_ESR_GL1  : i = 5; break;
        case APPLE_VIRT_VREG_M1_FAR_EL1  : case APPLE_VIRT_VREG_M1_FAR_GL1  : i = 6; break;
        default                          : return false;
    }
    field = live ? NULL : gxf_bank_field(env, gl, i);

    if (read) {
        value = live ? virt.ops->get_reg(cs, gxf_bank_regs[i]) : *field;
        if (VR_ENV("VR_CHECK_M1")) {
            // Debug: compare with what the slow path reads from the synced state.
            static unsigned reported;
            uint64_t slow;

            cpu_synchronize_state(cs);
            slow = *virt_vreg_field(env, vreg);
            if (slow != value && reported++ < 40) {
                fprintf(stderr, "M1-CHECK cpu%d vreg %d gl=%d guarded=%d fast=0x%" PRIx64 " slow=0x%" PRIx64 "\n",
                        cs->cpu_index, vreg, gl, (int)(env->gxf.gxf_status_el[1] & 1), value, slow);
            }
            if (rt != 31) { env->xregs[rt] = slow; }
            return true;
        }
        if (rt != 31) { virt.ops->set_xreg(cs, rt, value); }
        return true;
    }
    value = rt == 31 ? 0 : virt.ops->get_xreg(cs, rt);
    if (live) { virt.ops->set_reg(cs, gxf_bank_regs[i], value); }
    else {
        *field = value;
    }
    return true;
}

static bool virt_handle_sysreg_fast(CPUState* cs, CPUARMState* env, AppleVirtCPU* c, int vreg, bool read, uint32_t rt,
                                    bool after)
{
    uint64_t* field;
    uint64_t  value;

    switch (vreg) {
        case APPLE_VIRT_VREG_M1_TPIDR_EL1 ... APPLE_VIRT_VREG_M1_FAR_EL1:
        case APPLE_VIRT_VREG_M1_TPIDR_GL1:
        case APPLE_VIRT_VREG_M1_VBAR_GL1 :
        case APPLE_VIRT_VREG_M1_SPSR_GL1 :
        case APPLE_VIRT_VREG_M1_ESR_GL1  :
        case APPLE_VIRT_VREG_M1_ELR_GL1  :
        case APPLE_VIRT_VREG_M1_FAR_GL1  :
            if (VR_ENV("VR_NO_M1_FAST")) { return false; }    // debug: compare with the slow path
            return virt_m1_bank_fast(cs, env, vreg, read, rt);
        case APPLE_VIRT_VREG_M1_ASPSR_GL1: break;
        case APPLE_VIRT_VREG_SPRR_CONFIG_EL1   :
        case APPLE_VIRT_VREG_GXF_CONFIG_EL1    :
        case APPLE_VIRT_VREG_GXF_STATUS_EL1    :
        case APPLE_VIRT_VREG_GXF_ENTER_EL1     :
        case APPLE_VIRT_VREG_GXF_ABORT_EL1     :
        case APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0:
        case APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0:
        case APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1:
        case APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1: break;
        // While guarded, the vCPU holds the GL bank and the unguarded world's is only in `env`; ASPSR is only
        // accessible while guarded.
        case APPLE_VIRT_VREG_ASPSR_GL11:
        case APPLE_VIRT_VREG_SP_GL11   :
        case APPLE_VIRT_VREG_TPIDR_GL11:
        case APPLE_VIRT_VREG_VBAR_GL11 :
        case APPLE_VIRT_VREG_SPSR_GL11 :
        case APPLE_VIRT_VREG_ESR_GL11  :
        case APPLE_VIRT_VREG_ELR_GL11  :
        case APPLE_VIRT_VREG_FAR_GL11:
            if (!(env->gxf.gxf_status_el[1] & 1)) { return false; }
            break;
        // `env` holds the guest's keys and SCTLR and the vCPU those of the current EL, which APCTL and the kernel
        // key change.
        case APPLE_VIRT_VREG_APIAKEYLO_EL1 ... APPLE_VIRT_VREG_APGAKEYHI_EL1:
        case APPLE_VIRT_VREG_SCTLR_EL1                                      :
        case APPLE_VIRT_VREG_APCTL_EL1                                      :
        case APPLE_VIRT_VREG_KERNKEYLO_EL1                                  :
        case APPLE_VIRT_VREG_KERNKEYHI_EL1                                  : break;
        // The vCPU holds the shadows, `env` the guest's tables.
        case APPLE_VIRT_VREG_TTBR0_EL1:
        case APPLE_VIRT_VREG_TTBR1_EL1:
        case APPLE_VIRT_VREG_TCR_EL1:
            if (!read && !after) { return false; }
            break;
        default: return false;
    }

    field = virt_vreg_field(env, vreg);
    if (read) {
        if (rt != 31) { virt.ops->set_xreg(cs, rt, *field); }
        return true;
    }

    value = virt_vreg_write_value(env, vreg, rt == 31 ? 0 : virt.ops->get_xreg(cs, rt));
    switch (vreg) {
        case APPLE_VIRT_VREG_SCTLR_EL1: {
            int el = extract64(virt.ops->get_reg(cs, APPLE_VIRT_REG_CPSR), 2, 2);

            // The shadows are only loaded with the MMU on.
            if (c->shadowed && (value & ~env->cp15.sctlr_el[1] & SCTLR_M)) {
                if (!after) { return false; }
                env->cp15.sctlr_el[1] = value;
                gxf_reload_world_fast(cs, env, c, false);
                return true;
            }
            env->cp15.sctlr_el[1] = value;
            virt.ops->set_reg(cs, APPLE_VIRT_REG_SCTLR_EL1, pac_hw_sctlr(env, value, el));
            return true;
        }
        case APPLE_VIRT_VREG_APIAKEYLO_EL1 ... APPLE_VIRT_VREG_APGAKEYHI_EL1: {
            int k = vreg - APPLE_VIRT_VREG_APIAKEYLO_EL1;

            *field = value;
            if (c->pac_loaded) { virt.ops->set_reg(cs, APPLE_VIRT_REG_APIAKEYLO_EL1 + k, pac_key(env, k, c->pac_el)); }
            return true;
        }
        case APPLE_VIRT_VREG_APCTL_EL1    :
        case APPLE_VIRT_VREG_KERNKEYLO_EL1:
        case APPLE_VIRT_VREG_KERNKEYHI_EL1:
            if (value != *field) {
                *field = value;
                if (c->pac_loaded) { pac_install(cs, env, c, c->pac_el); }
            }
            return true;
        // Read-only.
        case APPLE_VIRT_VREG_GXF_STATUS_EL1    : return false;
        case APPLE_VIRT_VREG_SPRR_CONFIG_EL1   :
        case APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0:
        case APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0:
        case APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1:
        case APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1:
            if (value == *field) { return true; }
            if (!after) { return false; }
            *field = value;
            virt_sprr_mirror(env);
            gxf_reload_world_fast(cs, env, c, true);
            return true;
        case APPLE_VIRT_VREG_TTBR0_EL1:
        case APPLE_VIRT_VREG_TTBR1_EL1:
        case APPLE_VIRT_VREG_TCR_EL1:
            if (value == *field) { return true; }
            *field = value;
            if (vreg == APPLE_VIRT_VREG_TCR_EL1) { virt.ops->set_reg(cs, APPLE_VIRT_REG_TCR_EL1, value); }
            // The guest does its own TLB maintenance for a new translation.
            gxf_reload_world_fast(cs, env, c, false);
            return true;
        default: *field = value; return true;
    }
}

// The IMPLEMENTATION DEFINED registers the patcher gave the vregs from APPLE_VIRT_VREG_MAX on. Only ever appended to,
// so the vregs stay valid in images patched before a reset.
static struct
{
    uint32_t sysregs[APPLE_VIRT_VREG_LIMIT - APPLE_VIRT_VREG_MAX];
    uint32_t count;
} impdef;

bool apple_virt_rewrites_impdef(void) { return !virt.ops->traps_impdef; }

bool apple_virt_gic_interrupts(void) { return virt.ops->gic_interrupts; }

int apple_virt_impdef_vreg(uint32_t sysreg)
{
    uint32_t i;

    for (i = 0; i < impdef.count; i++) {
        if (impdef.sysregs[i] == sysreg) { return APPLE_VIRT_VREG_MAX + i; }
    }
    if (i == ARRAY_SIZE(impdef.sysregs)) { return -1; }
    impdef.sysregs[i] = sysreg;
    qatomic_store_release(&impdef.count, i + 1);
    return APPLE_VIRT_VREG_MAX + i;
}

bool apple_virt_hvc_impdef(uint16_t imm, uint32_t* op0, uint32_t* op1, uint32_t* crn, uint32_t* crm, uint32_t* op2)
{
    int      vreg = APPLE_VIRT_HVC_VREG(imm);
    uint32_t sysreg;

    if (!(imm & APPLE_VIRT_HVC_SYSREG) || vreg < APPLE_VIRT_VREG_MAX
        || vreg - APPLE_VIRT_VREG_MAX >= qatomic_load_acquire(&impdef.count))
    {
        return false;
    }
    sysreg = impdef.sysregs[vreg - APPLE_VIRT_VREG_MAX];
    *op0   = extract32(sysreg, 14, 2);
    *op1   = extract32(sysreg, 11, 3);
    *crn   = extract32(sysreg, 7, 4);
    *crm   = extract32(sysreg, 3, 4);
    *op2   = extract32(sysreg, 0, 3);
    return true;
}

bool apple_virt_handle_hvc_fast(CPUState* cs, uint16_t imm)
{
    CPUARMState*  env = cpu_env(cs);
    AppleVirtCPU* c   = virt_cpu(cs);

    // Only a lower-EL IRQ of a managed vCPU has anything to do.
    if (imm == APPLE_VIRT_HVC_IRQ_ENTRY || (imm == APPLE_VIRT_HVC_IRQ_LOWER && !c->managed)) { return true; }
    if (!c->managed || cs->vcpu_dirty) { return false; }

    if (imm == APPLE_VIRT_HVC_RESUME) {
        if (!c->flushing) { return false; }
        gxf_resume_fast(cs, env, c);
        return true;
    }
    if (imm == APPLE_VIRT_HVC_LOWER_ENTRY) {
        static int skip = -1;

        if (virt_el0_sprr_fast(cs, env, c)) { return true; }
        pac_load(cs, env, c, 1);
        // vphone-linux debug: step over the `hvc 0x13fc` behind it (see `ck_virt_vectors`).
        if (skip < 0) { skip = VR_ENV("VR_CATCH_CORRUPT"); }
        if (skip) { virt.ops->set_reg(cs, APPLE_VIRT_REG_PC, virt.ops->get_reg(cs, APPLE_VIRT_REG_PC) + 4); }
        return true;
    }
    if (imm == APPLE_VIRT_HVC_IRQ_LOWER) {
        pac_load(cs, env, c, 1);
        return true;
    }

    if (cs->singlestep_enabled) { return false; }
    if (imm == APPLE_VIRT_HVC_ERET) { return virt_eret_fast(cs, env, c); }
    if ((imm & APPLE_VIRT_HVC_GXF_MASK) == APPLE_VIRT_HVC_GENTER) { return gxf_genter_fast(cs, env, c, imm); }
    if ((imm & APPLE_VIRT_HVC_GXF_MASK) == APPLE_VIRT_HVC_GEXIT) { return gxf_gexit_fast(cs, env, c); }
    if (imm & APPLE_VIRT_HVC_SYSREG) {
        return virt_handle_sysreg_fast(cs, env, c, APPLE_VIRT_HVC_VREG(imm), APPLE_VIRT_HVC_READ(imm),
                                       APPLE_VIRT_HVC_RT(imm), true);
    }
    return false;
}

bool apple_virt_handle_sysreg_fast(CPUState* cs, uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2,
                                   bool read, uint32_t rt)
{
    int           vreg = apple_virt_vreg(op0, op1, crn, crm, op2);
    AppleVirtCPU* c    = virt_cpu(cs);

    if (vreg < 0 || !c->managed || cs->vcpu_dirty) { return false; }
    // All of them are EL1 registers, except SPRR_UPERM_BR0_EL0, which is restricted at EL0.
    if (extract64(virt.ops->get_reg(cs, APPLE_VIRT_REG_CPSR), 2, 2) != 1) { return false; }
    return virt_handle_sysreg_fast(cs, cpu_env(cs), c, vreg, read, rt, false);
}

bool apple_virt_sysreg_written(CPUState* cs, uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2)
{
    CPUARMState*  env = cpu_env(cs);
    AppleVirtCPU* c   = virt_cpu(cs);

    switch (apple_virt_vreg(op0, op1, crn, crm, op2)) {
        case APPLE_VIRT_VREG_SPRR_CONFIG_EL1   :
        case APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0:
        case APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0:
        case APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1:
        case APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1: break;
        default                                : return false;
    }
    if (!c->managed) { return false; }

    // The shadows bake the permissions into the TLB entries; the flush resumes after the `msr`.
    cpu_synchronize_state(cs);
    env->pc += 4;
    gxf_update_world(env, c, true);
    return true;
}

bool apple_virt_handle_hvc(CPUState* cs, uint16_t imm)
{
    CPUARMState*  env = cpu_env(cs);
    AppleVirtCPU* c   = virt_cpu(cs);
    GXFWorld      w;

    if (!c->managed) { return false; }

    if (imm & APPLE_VIRT_HVC_SYSREG) { return virt_handle_sysreg(env, c, imm); }

    if ((imm & APPLE_VIRT_HVC_GXF_MASK) == APPLE_VIRT_HVC_GENTER) {
        if (arm_is_guarded(env)) { return false; }
        env->exception.syndrome  = syn_aa64_genter(imm & ~APPLE_VIRT_HVC_GXF_MASK);
        env->exception.target_el = 1;
        cs->exception_index      = EXCP_GENTER;
        arm_cpu_do_interrupt(cs);
        gxf_update_world(env, c, false);
        return true;
    }

    if ((imm & APPLE_VIRT_HVC_GXF_MASK) == APPLE_VIRT_HVC_GEXIT) {
        if (!arm_is_guarded(env)) { return false; }
        aarch64_gxf_exit(env);
        gxf_update_world(env, c, false);
        return true;
    }

    if ((imm & APPLE_VIRT_HVC_TLBI_ASID_MASK) == APPLE_VIRT_HVC_TLBI_ASID) {
        uint32_t rt = APPLE_VIRT_HVC_RT(imm);

        WITH_QEMU_LOCK_GUARD(&virt.lock) { gxf_drop_asid(rt == 31 ? 0 : env->xregs[rt] >> 48); }
        w = gxf_env_world(env);
        gxf_flush_tlb(c, &w, true);
        gxf_env_apply(env, &w);
        return true;
    }

    switch (imm) {
        case APPLE_VIRT_HVC_LOWER_ENTRY:
        case APPLE_VIRT_HVC_IRQ_ENTRY  :
        case APPLE_VIRT_HVC_IRQ_LOWER:
            // The keys of the new EL are installed when the state is put.
            return true;
        case APPLE_VIRT_HVC_ERET: {
            bool     guarded = arm_is_guarded(env);
            uint64_t spsr    = guarded ? env->gxf.spsr_gl[1] : env->banked_spsr[BANK_SVC];

            if (extract64(spsr, 2, 2) > 1) { return false; }
            gxf_env_set_pstate(env, spsr & aarch64_pstate_valid_mask(&env_archcpu(env)->isar) & ~PSTATE_SS);
            env->pc = guarded ? env->gxf.elr_gl[1] : env->elr_el[1];
            return true;
        }
        case APPLE_VIRT_HVC_RESUME:
            if (!c->flushing) { return false; }
            c->flushing           = false;
            env->cp15.sctlr_el[1] = c->resume_sctlr;
            gxf_env_set_pstate(env, c->resume_pstate);
            env->pc = c->resume_pc;
            return true;
        default: return false;
    }
}

static void gxf_page_updated(hwaddr page, uint32_t off, uint32_t len)
{
    GXFPage* p                   = gxf_page(page);
    g_autoptr(GPtrArray) mirrors = NULL;

    if (p == NULL) { return; }

    // Resyncing can free mirrors of this page, so hold them all first.
    mirrors = g_ptr_array_copy(p->mirrors, NULL, NULL);
    for (guint i = 0; i < mirrors->len; i++) { ((GXFMirror*)g_ptr_array_index(mirrors, i))->refs += 1; }
    for (guint i = 0; i < mirrors->len; i++) {
        GXFMirror* m     = g_ptr_array_index(mirrors, i);
        hwaddr     start = MAX(page + off, m->ipa);
        hwaddr     end   = MIN(page + off + len, m->ipa + m->nents * sizeof(uint64_t));

        if (start < end) {
            uint32_t first = (start - m->ipa) / sizeof(uint64_t);
            uint32_t last  = (end - m->ipa - 1) / sizeof(uint64_t);

            gxf_mirror_sync_range(m, first, last - first + 1);
        }
    }
    for (guint i = 0; i < mirrors->len; i++) { gxf_mirror_put(g_ptr_array_index(mirrors, i)); }
}

// A store being emulated, on registers in the vCPU (`live`) or in a synced `env`.
typedef struct
{
    CPUState*    cs;
    CPUARMState* env;
    bool         live;
    uint64_t     page_va;
    uint8_t*     host;
    uint32_t     lo;
    uint32_t     hi;
} GXFStore;

// The current stack pointer, which is what register 31 means as a base.
static AppleVirtReg gxf_live_sp(GXFStore* st)
{
    uint64_t cpsr = virt.ops->get_reg(st->cs, APPLE_VIRT_REG_CPSR);

    return (cpsr & PSTATE_SP) && extract64(cpsr, 2, 2) != 0 ? APPLE_VIRT_REG_SP_EL1 : APPLE_VIRT_REG_SP_EL0;
}

static uint64_t gxf_xreg(GXFStore* st, int n, bool sp)
{
    if (n == 31 && !sp) { return 0; }
    if (!st->live) { return st->env->xregs[n]; }
    return n == 31 ? virt.ops->get_reg(st->cs, gxf_live_sp(st)) : virt.ops->get_xreg(st->cs, n);
}

static void gxf_set_xreg(GXFStore* st, int n, uint64_t value, bool sp)
{
    if (n == 31 && !sp) { return; }
    if (!st->live) { st->env->xregs[n] = value; }
    else if (n == 31) {
        virt.ops->set_reg(st->cs, gxf_live_sp(st), value);
    }
    else {
        virt.ops->set_xreg(st->cs, n, value);
    }
}

static void gxf_qreg(GXFStore* st, int n, uint64_t value[2])
{
    if (st->live) { virt.ops->get_qreg(st->cs, n, value); }
    else {
        value[0] = st->env->vfp.zregs[n].d[0];
        value[1] = st->env->vfp.zregs[n].d[1];
    }
}

// The host address of a naturally aligned access within the faulting page, or NULL to step over it instead.
static void* gxf_store_ptr(GXFStore* st, uint64_t va, uint32_t size)
{
    uint64_t off = va - st->page_va;

    if (off >= GXF_PAGE_SIZE || GXF_PAGE_SIZE - off < size || !QEMU_IS_ALIGNED(va, MIN(size, 8))) { return NULL; }
    st->lo = MIN(st->lo, off);
    st->hi = MAX(st->hi, off + size);
    return st->host + off;
}

static bool gxf_store_reg(GXFStore* st, uint64_t va, uint32_t size, bool simd, int n)
{
    uint8_t* ptr = gxf_store_ptr(st, va, size);
    uint64_t value[2];

    if (ptr == NULL) { return false; }
    if (simd) { gxf_qreg(st, n, value); }
    else {
        value[0] = gxf_xreg(st, n, false);
    }

    switch (size) {
        case 1: qatomic_set((uint8_t*)ptr, value[0]); break;
        case 2: qatomic_set((uint16_t*)ptr, cpu_to_le16(value[0])); break;
        case 4: qatomic_set((uint32_t*)ptr, cpu_to_le32(value[0])); break;
        case 8: qatomic_set((uint64_t*)ptr, cpu_to_le64(value[0])); break;
        case 16:
            qatomic_set((uint64_t*)ptr, cpu_to_le64(value[0]));
            qatomic_set((uint64_t*)ptr + 1, cpu_to_le64(value[1]));
            break;
        default: assert_not_reached();
    }
    return true;
}

// The access size of a single-register store, or 0 for a load.
static uint32_t gxf_ldst_size(uint32_t insn, bool simd)
{
    uint32_t size = extract32(insn, 30, 2);
    uint32_t opc  = extract32(insn, 22, 2);

    if (opc == 0) { return 1 << size; }
    if (simd && opc == 2 && size == 0) { return 16; }
    return 0;
}

static uint64_t gxf_extend_reg(uint64_t value, uint32_t option)
{
    switch (option) {
        case 2 : return (uint32_t)value;
        case 6 : return (int64_t)(int32_t)value;
        default: return value;
    }
}

static uint64_t gxf_atomic_load(void* ptr, uint32_t size)
{
    switch (size) {
        case 1 : return qatomic_read((uint8_t*)ptr);
        case 2 : return le16_to_cpu(qatomic_read((uint16_t*)ptr));
        case 4 : return le32_to_cpu(qatomic_read((uint32_t*)ptr));
        default: return le64_to_cpu(qatomic_read((uint64_t*)ptr));
    }
}

static uint64_t gxf_atomic_cmpxchg(void* ptr, uint32_t size, uint64_t cmp, uint64_t new)
{
    switch (size) {
        case 1 : return qatomic_cmpxchg((uint8_t*)ptr, (uint8_t)cmp, (uint8_t)new);
        case 2 : return le16_to_cpu(qatomic_cmpxchg((uint16_t*)ptr, cpu_to_le16(cmp), cpu_to_le16(new)));
        case 4 : return le32_to_cpu(qatomic_cmpxchg((uint32_t*)ptr, cpu_to_le32(cmp), cpu_to_le32(new)));
        default: return le64_to_cpu(qatomic_cmpxchg((uint64_t*)ptr, cpu_to_le64(cmp), cpu_to_le64(new)));
    }
}

static bool gxf_emulate_atomic(GXFStore* st, uint32_t insn)
{
    uint32_t size  = 1 << extract32(insn, 30, 2);
    uint32_t rs    = extract32(insn, 16, 5);
    uint32_t rn    = extract32(insn, 5, 5);
    uint32_t rt    = extract32(insn, 0, 5);
    uint64_t mask  = size == 8 ? UINT64_MAX : (1ULL << (size * 8)) - 1;
    uint64_t value = gxf_xreg(st, rs, false) & mask;
    uint32_t opc   = extract32(insn, 12, 3);
    bool     swp   = extract32(insn, 15, 1);
    void*    ptr;
    uint64_t old;
    uint64_t seen;

    if (swp && opc != 0) { return false; }
    ptr = gxf_store_ptr(st, gxf_xreg(st, rn, true), size);
    if (ptr == NULL) { return false; }

    for (old = gxf_atomic_load(ptr, size);; old = seen) {
        int64_t  a = sextract64(old, 0, size * 8);
        int64_t  b = sextract64(value, 0, size * 8);
        uint64_t new;

        switch (swp ? 8 : opc) {
            case 0 : new = old + value; break;                  // ADD
            case 1 : new = old & ~value; break;                 // CLR
            case 2 : new = old ^ value; break;                  // EOR
            case 3 : new = old | value; break;                  // SET
            case 4 : new = a > b ? old : value; break;          // SMAX
            case 5 : new = a < b ? old : value; break;          // SMIN
            case 6 : new = old > value ? old : value; break;    // UMAX
            case 7 : new = old < value ? old : value; break;    // UMIN
            default: new = value; break;                        // SWP
        }
        seen = gxf_atomic_cmpxchg(ptr, size, old, new & mask);
        if (seen == old) { break; }
    }

    gxf_set_xreg(st, rt, old, false);
    return true;
}

static bool gxf_emulate_cas(GXFStore* st, uint32_t insn)
{
    uint32_t size = 1 << extract32(insn, 30, 2);
    uint32_t rs   = extract32(insn, 16, 5);
    uint32_t rn   = extract32(insn, 5, 5);
    uint32_t rt   = extract32(insn, 0, 5);
    void*    ptr  = gxf_store_ptr(st, gxf_xreg(st, rn, true), size);

    if (ptr == NULL) { return false; }
    gxf_set_xreg(st, rs, gxf_atomic_cmpxchg(ptr, size, gxf_xreg(st, rs, false), gxf_xreg(st, rt, false)), false);
    return true;
}

// Emulates the store that hit a write-protected page table, like m1n1, so the page never becomes writable.
static bool gxf_emulate_store(GXFStore* st, uint32_t insn)
{
    bool     simd = extract32(insn, 26, 1);
    uint32_t rn   = extract32(insn, 5, 5);
    uint32_t rt   = extract32(insn, 0, 5);
    uint64_t base;
    uint32_t size;

    // stlr{b,h,}
    if ((insn & 0x3FFFFC00) == 0x089FFC00) {
        return gxf_store_reg(st, gxf_xreg(st, rn, true), 1 << extract32(insn, 30, 2), false, rt);
    }

    // cas{,a,l,al}{b,h,}
    if ((insn & 0x3FA07C00) == 0x08A07C00) { return gxf_emulate_cas(st, insn); }

    // ld<op>{,a,l,al}{b,h,}, swp{,a,l,al}{b,h,}
    if ((insn & 0x3F200C00) == 0x38200000) { return gxf_emulate_atomic(st, insn); }

    // dc zva
    if ((insn & 0xFFFFFFE0) == 0xD50B7420) {
        uint64_t dczid;
        uint32_t block;

        __asm__("mrs %0, dczid_el0" : "=r"(dczid));
        block = 4 << (dczid & 0xF);
        base  = gxf_xreg(st, rt, false) & ~(uint64_t)(block - 1);
        for (uint32_t i = 0; i < block; i += 8) {
            uint64_t* ptr = gxf_store_ptr(st, base + i, 8);

            if (ptr == NULL) { return false; }
            qatomic_set(ptr, 0);
        }
        return true;
    }

    // str{b,h,} / str (SIMD&FP), unsigned offset
    if ((insn & 0x3B000000) == 0x39000000) {
        size = gxf_ldst_size(insn, simd);
        if (size == 0) { return false; }
        return gxf_store_reg(st, gxf_xreg(st, rn, true) + extract32(insn, 10, 12) * size, size, simd, rt);
    }

    // stur, str pre/post-index, str register offset
    if ((insn & 0x3B000000) == 0x38000000) {
        uint32_t type = extract32(insn, 10, 2);
        uint64_t va;
        uint64_t wb;

        size = gxf_ldst_size(insn, simd);
        if (size == 0) { return false; }
        base = gxf_xreg(st, rn, true);

        if (extract32(insn, 21, 1)) {
            uint32_t rm     = extract32(insn, 16, 5);
            uint32_t option = extract32(insn, 13, 3);
            uint64_t offset = gxf_extend_reg(gxf_xreg(st, rm, false), option);

            if (type != 2) { return false; }
            if (extract32(insn, 12, 1)) { offset <<= ctz32(size); }
            return gxf_store_reg(st, base + offset, size, simd, rt);
        }

        wb = base + sextract32(insn, 12, 9);
        va = type == 1 ? base : wb;
        if (!gxf_store_reg(st, va, size, simd, rt)) { return false; }
        if (type == 1 || type == 3) { gxf_set_xreg(st, rn, wb, true); }
        return true;
    }

    // stp / stnp (and SIMD&FP)
    if ((insn & 0x3A400000) == 0x28000000) {
        uint32_t opc  = extract32(insn, 30, 2);
        uint32_t type = extract32(insn, 23, 2);
        uint32_t rt2  = extract32(insn, 10, 5);
        uint64_t wb;
        uint64_t va;

        size = simd ? 4 << opc : (opc == 0 ? 4 : opc == 2 ? 8 : 0);
        if (size == 0 || size > 16) { return false; }

        base = gxf_xreg(st, rn, true);
        wb   = base + (int64_t)sextract32(insn, 15, 7) * size;
        va   = type == 1 ? base : wb;
        if (!gxf_store_reg(st, va, size, simd, rt) || !gxf_store_reg(st, va + size, size, simd, rt2)) { return false; }
        if (type == 1 || type == 3) { gxf_set_xreg(st, rn, wb, true); }
        return true;
    }

    return false;
}

// The shadow tables are only in the pool, which isn't guest memory.
static uint64_t gxf_table_read(hwaddr pa)
{
    if (pa >= virt.pool_base && pa < virt.pool_end) {
        return le64_to_cpu(qatomic_read((uint64_t*)(virt.pool_host + (pa - virt.pool_base))));
    }
    return address_space_ldq_le(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, NULL);
}

static bool gxf_live_translate(CPUState* cs, uint64_t va, hwaddr* pa)
{
    uint64_t tcr;
    uint64_t ttbr;
    hwaddr   table;
    uint32_t va_bits;
    int      idx;
    int      level;

    if (!(virt.ops->get_reg(cs, APPLE_VIRT_REG_SCTLR_EL1) & SCTLR_M)) {
        *pa = va;
        return true;
    }

    tcr     = virt.ops->get_reg(cs, APPLE_VIRT_REG_TCR_EL1);
    idx     = extract64(va, 55, 1);
    va_bits = 64 - extract64(tcr, idx ? 16 : 0, 6);
    ttbr    = virt.ops->get_reg(cs, idx ? APPLE_VIRT_REG_TTBR1_EL1 : APPLE_VIRT_REG_TTBR0_EL1);
    table   = ttbr & GXF_TTBR_BADDR;
    if (extract64(tcr, idx ? 30 : 14, 2) != (idx ? 1 : 2) || va_bits > 48) { return false; }

    for (level = 0; level < 3 && va_bits <= gxf_lvl_off[level]; level++) { }
    for (; level <= 3; level++) {
        uint32_t bits = level == 0 || va_bits <= gxf_lvl_off[level - 1] ? va_bits - gxf_lvl_off[level] : 11;
        uint64_t d    = gxf_table_read(table + extract64(va, gxf_lvl_off[level], bits) * sizeof(uint64_t));

        if (!(d & PTE_VALID)) { return false; }
        if (level == 3 || !(d & PTE_TABLE)) {
            uint64_t size = 1ULL << gxf_lvl_off[level];

            if (level == 3 && !(d & PTE_TABLE)) { return false; }
            *pa = (d & GXF_OA_MASK & ~(size - 1)) | (va & (size - 1));
            return true;
        }
        table = d & GXF_OA_MASK;
    }
    return false;
}

// vphone-linux debug: install the kernel's PAC keys, as a lower-EL entry does.
/*
 * vphone-linux: the kernel's fast path for the patched exception entries and returns
 * (`struct kvm_arm_apple_fast` in linux-aurora-kvm's arch/arm64/include/uapi/asm/kvm.h), which
 * installs the PAC key sets and SCTLR published here as `pac_load` would, without an exit.
 */
typedef struct {
    uint64_t magic;
    uint64_t pstate_mask;
    uint64_t keys[2][10];
    uint64_t sctlr[2];
    uint64_t pac_el;
    uint64_t handled;
    uint64_t resume_valid;
    uint64_t resume_pc;
    uint64_t resume_pstate;
    uint64_t resume_sctlr;
    uint64_t resume_el;
    uint32_t nreads;
    uint32_t pad;
    uint16_t read_imm[16];
    uint64_t read_value[16];
    uint64_t el0_ok;
    uint32_t nsites;
    uint32_t pad2;
    uint64_t site_va[32];
    uint16_t site_imm[32];
    uint8_t  site_rt[32];
    uint64_t disable;
    uint64_t guarded;
    uint64_t m1_bank_ok;
} AppleVirtFast;


// The registers whose reads are only `env`'s field (`virt_handle_sysreg_fast`), which only this side
// writes: the kernel answers them from the values published before the vCPU runs.
static const int apple_virt_fast_reads[] = {
    APPLE_VIRT_VREG_GXF_STATUS_EL1,     APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0, APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0,
    APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1, APPLE_VIRT_VREG_M1_ASPSR_GL1,
};

#define APPLE_VIRT_FAST_MAGIC 0x3130545341464150ULL

// Before the vCPU runs: what an entry or a return installs, or nothing for a vCPU whose entries and
// returns need this side (unmanaged, single-stepping, flushing, the corruption catcher's).
void apple_virt_fast_publish(CPUState* cs, void* area)
{
    AppleVirtFast* f   = area;
    AppleVirtCPU*  c   = virt_cpu(cs);
    CPUARMState*   env = cpu_env(cs);

    if (!c->managed || cs->singlestep_enabled || VR_ENV("VR_CATCH_CORRUPT") || VR_ENV("VR_NO_FAST")) {
        f->magic = 0;
        return;
    }
    // Debug: VR_FAST_DISABLE=<mask> leaves traps to this side (bit imm - 0x1340, 4 resume, 5 reads).
    {
        static int64_t disable = -1;

        if (disable < 0) { disable = getenv("VR_FAST_DISABLE") ? strtoull(getenv("VR_FAST_DISABLE"), NULL, 0) : 0; }
        f->disable = disable;
    }
    // A pending flush resumes as `gxf_resume_fast` does it, with VR_FAST_RESUME=1 (and not VR_FAST_DISABLE's bit
    // 4): an iOS guest's lock-screen wallpaper rendered black with the kernel resuming, cause not found yet.
    f->resume_valid = c->flushing && VR_ENV("VR_FAST_RESUME") && !(f->disable & (1 << 4));
    f->pad          = f->resume_valid;    // what this side published, which the kernel leaves
    if (c->flushing) {
        int el = extract32(c->resume_pstate, 2, 2);

        f->resume_pc     = c->resume_pc;
        f->resume_pstate = c->resume_pstate;
        f->resume_el     = el;
        f->resume_sctlr  = pac_hw_sctlr(env, c->resume_sctlr, el);
    }
    f->nreads = 0;
    if (!VR_ENV("VR_NO_FAST_READS")) {
        for (int i = 0; i < ARRAY_SIZE(apple_virt_fast_reads); i++) {
            int vreg = apple_virt_fast_reads[i];

            f->read_imm[i]   = APPLE_VIRT_HVC_SYSREG | (vreg << 6) | (1 << 5);
            f->read_value[i] = *virt_vreg_field(env, vreg);
        }
        f->nreads = ARRAY_SIZE(apple_virt_fast_reads);
    }
    // EL0's SPRR reads, unguarded (`virt_el0_sprr_fast`'s condition), at the addresses seen so far.
    f->el0_ok = f->nreads != 0 && !(env->gxf.gxf_status_el[1] & 1);
    {
        uint32_t n = qatomic_load_acquire(&apple_virt_el0_sites.n);

        for (uint32_t i = f->nsites; i < n; i++) {
            f->site_va[i]  = apple_virt_el0_sites.va[i];
            f->site_imm[i] = apple_virt_el0_sites.imm[i];
            f->site_rt[i]  = apple_virt_el0_sites.rt[i];
        }
        f->nsites = n;
    }
    // While a flush is pending `env` holds the stub's SCTLR (its MMU off); the entries and returns
    // after the kernel resumes run with the one it resumes to.
    uint64_t sctlr = c->flushing ? c->resume_sctlr : env->cp15.sctlr_el[1];

    for (int el = 0; el < 2; el++) {
        for (int k = 0; k < ARRAY_SIZE(c->guest_keys); k++) { f->keys[el][k] = pac_key(env, k, el); }
        f->sctlr[el] = pac_hw_sctlr(env, sctlr, el);
    }
    f->pstate_mask = aarch64_pstate_valid_mask(&env_archcpu(env)->isar) & ~PSTATE_SS;
    // An M1-style monitor's own exception registers while guarded (`virt_m1_bank_fast`'s live case).
    f->guarded    = env->gxf.gxf_status_el[1] & 1;
    f->m1_bank_ok = apple_virt_m1_gl_names && !VR_ENV("VR_NO_M1_FAST");
    f->pac_el      = c->pac_loaded ? c->pac_el : 2;
    f->magic       = APPLE_VIRT_FAST_MAGIC;
}

// After the vCPU ran: the key set the kernel left it holding.
void apple_virt_fast_collect(CPUState* cs, void* area)
{
    AppleVirtFast* f = area;
    AppleVirtCPU*  c = virt_cpu(cs);

    if (f->magic != APPLE_VIRT_FAST_MAGIC) { return; }
    if (f->pac_el < 2) {
        c->pac_loaded = true;
        c->pac_el     = f->pac_el;
    }
    // The kernel resumed from the flush stub.
    if (c->flushing && f->pad && !f->resume_valid) {
        cpu_env(cs)->cp15.sctlr_el[1] = c->resume_sctlr;
        c->flushing                   = false;
    }
}

void apple_virt_fast_publish(CPUState* cs, void* area);
void apple_virt_fast_collect(CPUState* cs, void* area);
void apple_virt_debug_pac_el1(CPUState* cs);
void apple_virt_debug_pac_el1(CPUState* cs) { pac_load(cs, cpu_env(cs), virt_cpu(cs), 1); }

// vphone-linux debug: translate `va` as the vCPU's hardware does (through the shadows).
bool apple_virt_debug_translate(CPUState* cs, uint64_t va, hwaddr* pa);
bool apple_virt_debug_translate(CPUState* cs, uint64_t va, hwaddr* pa) { return gxf_live_translate(cs, va, pa); }

static bool gxf_fetch_insn(GXFStore* st, uint64_t pc, uint32_t* insn)
{
    hwaddr pa;

    if (!st->live) { return cpu_memory_rw_debug(st->cs, pc, insn, sizeof(*insn), false) == 0; }
    if (!gxf_live_translate(st->cs, pc, &pa)) { return false; }
    *insn = address_space_ldl_le(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, NULL);
    return true;
}

static bool gxf_try_emulate(GXFStore* st, GXFPage* p)
{
    uint64_t pc = st->live ? virt.ops->get_reg(st->cs, APPLE_VIRT_REG_PC) : st->env->pc;
    uint32_t insn;

    st->lo = GXF_PAGE_SIZE;
    st->hi = 0;
    if (!gxf_fetch_insn(st, pc, &insn) || !gxf_emulate_store(st, insn)) { return false; }

    if (st->live) { virt.ops->set_reg(st->cs, APPLE_VIRT_REG_PC, pc + 4); }
    else {
        st->env->pc = pc + 4;
    }
    if (st->lo < st->hi) { gxf_page_updated(p->ipa, st->lo, st->hi - st->lo); }
    return true;
}

typedef enum
{
    GXF_FAULT_NOT_TABLE,
    GXF_FAULT_HANDLED,
    GXF_FAULT_NEEDS_SYNC,
} GXFFaultResult;

// Emulates a store to a guest page table at `ipa`, or steps over it once the state is synced.
static GXFFaultResult gxf_write_fault(CPUState* cs, uint64_t va, hwaddr ipa)
{
    CPUARMState*  env = cpu_env(cs);
    AppleVirtCPU* c   = virt_cpu(cs);
    GXFPage*      p;
    GXFStore      st;

    QEMU_LOCK_GUARD(&virt.lock);

    p = gxf_page(ipa);
    // The page stopped being a table and became writable after the store faulted, so it is retried.
    if (p == NULL) {
        return g_hash_table_contains(virt.unprotected, GSIZE_TO_POINTER(ipa & ~(hwaddr)(GXF_PAGE_SIZE - 1))) ?
                   GXF_FAULT_HANDLED :
                   GXF_FAULT_NOT_TABLE;
    }

    st = (GXFStore){
        .cs      = cs,
        .env     = env,
        .live    = !cs->vcpu_dirty,
        .page_va = va & ~(uint64_t)(GXF_PAGE_SIZE - 1),
        .host    = gxf_host_ptr(p->ipa, GXF_PAGE_SIZE),
    };
    if (c->nr_step_pages == 0 && st.host != NULL && gxf_try_emulate(&st, p)) { return GXF_FAULT_HANDLED; }
    if (st.live) { return GXF_FAULT_NEEDS_SYNC; }

    if (virt.ops->trap_debug == NULL) {
        error_report("apple-virt: can't step over an unemulated store to a page table at 0x%" PRIx64, env->pc);
        abort();
    }
    warn_report_once("apple-virt: stepping over an unemulated store at 0x%" PRIx64, env->pc);

    // Let the store through while it is stepped.
    if (p->protected) {
        gxf_protect(p->ipa, true);
        p->protected = false;
    }

    for (uint32_t i = 0; i < c->nr_step_pages; i++) {
        if (c->step_pages[i] == p->ipa) { return GXF_FAULT_HANDLED; }
    }
    if (c->nr_step_pages == ARRAY_SIZE(c->step_pages)) {
        error_report("apple-virt: a store at 0x%" PRIx64 " hits more than two guest page tables", env->pc);
        abort();
    }
    c->step_pages[c->nr_step_pages++]  = p->ipa;
    p->steppers                       += 1;
    if (c->nr_step_pages > 1) { return GXF_FAULT_HANDLED; }

    // Step over the store with interrupts masked, then resync in `apple_virt_handle_step`.
    c->step_mdscr        = env->cp15.mdscr_el1;
    c->step_pstate       = pstate_read(env);
    env->cp15.mdscr_el1 |= GXF_MDSCR_SS;
    pstate_write(env, c->step_pstate | PSTATE_SS | PSTATE_DAIF);
    virt.ops->trap_debug(cs, true);
    return GXF_FAULT_HANDLED;
}

bool apple_virt_handle_write_fault(CPUState* cs, uint64_t va, hwaddr ipa)
{
    if (!virt_cpu(cs)->managed) { return false; }

    switch (gxf_write_fault(cs, va, ipa)) {
        case GXF_FAULT_NOT_TABLE : return false;
        case GXF_FAULT_HANDLED   : return true;
        case GXF_FAULT_NEEDS_SYNC: break;
    }

    // Syncing can take the lock, so it happens outside it. If the page stopped being a table meanwhile, the store is
    // retried.
    cpu_synchronize_state(cs);
    gxf_write_fault(cs, va, ipa);
    return true;
}

bool apple_virt_handle_step(CPUState* cs)
{
    CPUARMState*  env  = cpu_env(cs);
    AppleVirtCPU* c    = virt_cpu(cs);
    uint32_t      mask = PSTATE_SS | PSTATE_DAIF;

    if (c->nr_step_pages == 0) { return false; }

    // Syncing can take the lock.
    cpu_synchronize_state(cs);
    QEMU_LOCK_GUARD(&virt.lock);

    env->cp15.mdscr_el1 = c->step_mdscr;
    pstate_write(env, (pstate_read(env) & ~mask) | (c->step_pstate & mask));
    virt.ops->trap_debug(cs, false);

    for (uint32_t i = 0; i < c->nr_step_pages; i++) {
        GXFPage* p = gxf_page(c->step_pages[i]);

        p->steppers -= 1;
        if (p->mirrors->len == 0) { gxf_page_put(p); }
        else {
            gxf_page_updated(p->ipa, 0, GXF_PAGE_SIZE);
        }
    }
    c->nr_step_pages = 0;
    return true;
}

static void gxf_swap_env_bank(CPUARMState* env)
{
    for (int i = 0; i < ARRAY_SIZE(gxf_bank_regs); i++) {
        uint64_t* el  = gxf_bank_field(env, false, i);
        uint64_t* gl  = gxf_bank_field(env, true, i);
        uint64_t  tmp = *el;

        *el = *gl;
        *gl = tmp;
    }
}

void apple_virt_sync_begin(CPUState* cs)
{
    CPUARMState*  env = cpu_env(cs);
    AppleVirtCPU* c   = virt_cpu(cs);
    int           el  = arm_current_el(env);

    if (!c->managed) { return; }

    // The accelerator's vCPU state can only be changed from its thread.
    if (c->untrap_debug) {
        virt.ops->trap_debug(cs, false);
        c->untrap_debug = false;
    }

    // While guarded, the vCPU's `_EL1` registers hold the GL1 bank.
    c->sync_guarded = env->gxf.gxf_status_el[1] & 1;
    if (c->sync_guarded) { gxf_swap_env_bank(env); }

    for (int k = 0; k < ARRAY_SIZE(c->guest_keys); k++) { c->guest_keys[k] = *pac_key_field(env, k); }
    for (int k = 0; k < ARRAY_SIZE(c->guest_keys); k++) { *pac_key_field(env, k) = pac_key(env, k, el); }
    c->guest_sctlr        = env->cp15.sctlr_el[1];
    env->cp15.sctlr_el[1] = pac_hw_sctlr(env, c->guest_sctlr, el);
    // A vCPU reset in the TLB flush stub left it.
    if (c->flushing && env->pc - virt.stub > 0x30) { c->flushing = false; }
    // The TLB flush stub runs with the MMU off.
    if (c->flushing) { env->cp15.sctlr_el[1] &= ~SCTLR_M; }
    c->pac_loaded = true;
    c->pac_el     = el;

    // The vCPU was reset.
    if (c->shadowed && !gxf_wants_shadows(env)) {
        QEMU_LOCK_GUARD(&virt.lock);
        gxf_release(c);
    }

    if (c->shadowed) {
        c->guest_ttbr[0]      = env->cp15.ttbr0_el[1];
        c->guest_ttbr[1]      = env->cp15.ttbr1_el[1];
        env->cp15.ttbr0_el[1] = c->hw_ttbr[0];
        env->cp15.ttbr1_el[1] = c->hw_ttbr[1];
    }
}

void apple_virt_sync_end(CPUState* cs)
{
    CPUARMState*  env = cpu_env(cs);
    AppleVirtCPU* c   = virt_cpu(cs);

    if (!c->managed) { return; }

    for (int k = 0; k < ARRAY_SIZE(c->guest_keys); k++) { *pac_key_field(env, k) = c->guest_keys[k]; }
    env->cp15.sctlr_el[1] = c->guest_sctlr;

    if (c->shadowed) {
        env->cp15.ttbr0_el[1] = c->guest_ttbr[0];
        env->cp15.ttbr1_el[1] = c->guest_ttbr[1];
    }

    if (c->sync_guarded) { gxf_swap_env_bank(env); }
}

void apple_virt_manage_cpu(CPUState* cs)
{
    if (virt.ops == NULL) {
        error_report("apple-virt: this accelerator can't run Apple's PAC and GXF");
        exit(1);
    }
    virt_cpu(cs)->managed = true;
}

static gboolean gxf_page_drop(gpointer key, gpointer value, gpointer opaque)
{
    gxf_page_free(value);
    return true;
}

static void gxf_reset(void)
{
    for (int i = 0; i < VIRT_MAX_CPUS; i++) {
        AppleVirtCPU* c = &virt.cpus[i];

        gxf_release(c);
        c->untrap_debug  = c->nr_step_pages != 0;
        c->nr_step_pages = 0;
        c->flushing      = false;
    }
    while (virt.roots->len != 0) { gxf_root_free(g_ptr_array_index(virt.roots, 0)); }
    assert_cmpuint(g_hash_table_size(virt.mirrors), ==, 0);
    // Pages of stores that were being stepped.
    g_hash_table_foreach_remove(virt.pages, gxf_page_drop, NULL);
    g_hash_table_remove_all(virt.unprotected);
    g_ptr_array_set_size(virt.perms, 0);
    g_queue_clear(&virt.pool_free);
    virt.pool_cursor = virt.pool_base + 2 * GXF_PAGE_SIZE;
    virt.pool_used   = 0;
}

static void apple_virt_reset(void* opaque)
{
    QEMU_LOCK_GUARD(&virt.lock);
    gxf_reset();
}

void apple_virt_set_ops(const AppleVirtOps* ops, hwaddr ipa_end)
{
    // tlbi vmalle1[is]; dsb nsh/ish; isb; hvc #RESUME
    uint32_t stub[] = {
        // tlbi vmalle1; dsb nsh; nop; nop; isb; trap  (and the inner-shareable forms). No
        // instruction-cache invalidate: it did not change the skipped-hvc hangs, and the stub runs
        // tens of thousands of times a second.
        0xD508871F, 0xD503379F, 0xD503201F, 0xD503201F, 0xD5033FDF, 0,
        0xD508831F, 0xD5033B9F, 0xD503201F, 0xD503201F, 0xD5033FDF, 0,
    };

    virt.ops = ops;
    qemu_mutex_init(&virt.lock);
    qemu_mutex_init(&apple_virt_el0_sites.lock);
    virt.perms       = g_ptr_array_new_with_free_func(g_free);
    virt.mirrors     = g_hash_table_new(gxf_mirror_hash, gxf_mirror_equal);
    virt.pages       = g_hash_table_new(g_int64_hash, g_int64_equal);
    virt.unprotected = g_hash_table_new(g_direct_hash, g_direct_equal);
    virt.roots       = g_ptr_array_new();

    virt.pool_host = qemu_memalign(GXF_PAGE_SIZE, GXF_POOL_SIZE);
    virt.pool_base = ipa_end - GXF_POOL_SIZE;
    virt.pool_end  = ipa_end;
    virt.stub      = virt.pool_base;
    virt.empty     = virt.pool_base + GXF_PAGE_SIZE;
    memset(virt.pool_host, 0, 2 * GXF_PAGE_SIZE);
    stub[5] = stub[11] = apple_virt_trap_insn(APPLE_VIRT_HVC_RESUME);
    for (size_t i = 0; i < ARRAY_SIZE(stub); i++) { stl_le_p(virt.pool_host + i * 4, stub[i]); }
    // The stub page is executed, the rest only walked.
    ops->map(virt.pool_host, virt.pool_base, GXF_PAGE_SIZE, true);
    ops->map(virt.pool_host + GXF_PAGE_SIZE, virt.pool_base + GXF_PAGE_SIZE, GXF_POOL_SIZE - GXF_PAGE_SIZE, false);

    gxf_reset();
    qemu_register_reset(apple_virt_reset, NULL);
}
