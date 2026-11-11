/*
 * Pages of vCPU-private address spaces for hardware accelerators
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "accel/accel-private.h"

/*
 * Pages of a vCPU's own address space that system memory leaves empty, mapped on first access. They are dropped when
 * that address space changes there, its IOMMU invalidates them, or system memory maps something there.
 */
typedef struct AccelPrivateSpace AccelPrivateSpace;

typedef struct
{
    IOMMUNotifier      n;
    IOMMUMemoryRegion* mr;
    AccelPrivateSpace* space;
} AccelPrivateIOMMU;

struct AccelPrivateSpace
{
    AddressSpace*  as;
    MemoryListener listener;
    GPtrArray*     iommus;
};

typedef struct
{
    hwaddr             ipa;
    AccelPrivateSpace* space;
    IOMMUMemoryRegion* iommu;
    hwaddr             iova;
} AccelPrivatePage;

typedef struct
{
    AccelPrivateSpace* space;
    IOMMUMemoryRegion* iommu;
    hwaddr             start;
    hwaddr             last;
    bool               by_iova;
} AccelPrivateRange;

static const AccelPrivateOps* accel_private_ops;
static QemuMutex              accel_private_lock;
static GPtrArray*             accel_private_spaces;
static GHashTable*            accel_private_pages;

static gboolean accel_private_page_drop(gpointer key, gpointer value, gpointer opaque)
{
    AccelPrivatePage*        page  = value;
    const AccelPrivateRange* range = opaque;
    hwaddr                   addr  = range->by_iova ? page->iova : page->ipa;

    if ((range->space != NULL && page->space != range->space) || (range->iommu != NULL && page->iommu != range->iommu)
        || addr + qemu_real_host_page_size() - 1 < range->start || addr > range->last)
    {
        return false;
    }

    accel_private_ops->unmap(page->ipa, qemu_real_host_page_size());
    g_free(page);
    return true;
}

static void accel_private_drop(AccelPrivateRange range)
{
    QEMU_LOCK_GUARD(&accel_private_lock);

    g_hash_table_foreach_remove(accel_private_pages, accel_private_page_drop, &range);
}

static void accel_private_drop_section(AccelPrivateSpace* space, MemoryRegionSection* section)
{
    accel_private_drop((AccelPrivateRange){
        .space = space,
        .start = section->offset_within_address_space,
        .last  = section->offset_within_address_space + int128_get64(section->size) - 1,
    });
}

static void accel_private_iommu_notify(IOMMUNotifier* n, IOMMUTLBEntry* entry)
{
    AccelPrivateIOMMU* iommu = container_of(n, AccelPrivateIOMMU, n);

    accel_private_drop((AccelPrivateRange){
        .space   = iommu->space,
        .iommu   = iommu->mr,
        .start   = entry->iova,
        .last    = entry->iova + MIN(entry->addr_mask, HWADDR_MAX - entry->iova),
        .by_iova = true,
    });
}

static void accel_private_region_del(MemoryListener* listener, MemoryRegionSection* section)
{ accel_private_drop_section(container_of(listener, AccelPrivateSpace, listener), section); }

static AccelPrivateSpace* accel_private_space(AddressSpace* as)
{
    AccelPrivateSpace* space;

    for (guint i = 0; i < accel_private_spaces->len; i++) {
        space = g_ptr_array_index(accel_private_spaces, i);
        if (space->as == as) { return space; }
    }

    space           = g_new0(AccelPrivateSpace, 1);
    space->as       = as;
    space->iommus   = g_ptr_array_new();
    space->listener = (MemoryListener){
        .name       = "accel-private",
        .region_del = accel_private_region_del,
        .priority   = MEMORY_LISTENER_PRIORITY_ACCEL,
    };
    memory_listener_register(&space->listener, as);
    g_ptr_array_add(accel_private_spaces, space);
    return space;
}

static void accel_private_watch_iommu(AccelPrivateSpace* space, IOMMUMemoryRegion* mr, int iommu_idx)
{
    AccelPrivateIOMMU* iommu;

    for (guint i = 0; i < space->iommus->len; i++) {
        iommu = g_ptr_array_index(space->iommus, i);
        if (iommu->mr == mr) { return; }
    }

    iommu        = g_new0(AccelPrivateIOMMU, 1);
    iommu->mr    = mr;
    iommu->space = space;
    iommu_notifier_init(&iommu->n, accel_private_iommu_notify, IOMMU_NOTIFIER_UNMAP, 0, HWADDR_MAX, iommu_idx);
    memory_region_register_iommu_notifier(MEMORY_REGION(mr), &iommu->n, &error_fatal);
    g_ptr_array_add(space->iommus, iommu);
}

bool accel_private_map_page(AddressSpace* as, hwaddr ipa, bool is_write)
{
    uint64_t            page_size = qemu_real_host_page_size();
    hwaddr              base      = ipa & ~(page_size - 1);
    IOMMUMemoryRegion*  iommu     = NULL;
    hwaddr              iova      = 0;
    bool                writable  = true;
    MemoryRegionSection section;
    AccelPrivateSpace*  space;
    AccelPrivatePage*   page;

    if (as == &address_space_memory) { return false; }

    WITH_RCU_READ_LOCK_GUARD()
    {
        bool candidate;

        section   = memory_region_find(as->root, base, page_size);
        candidate = section.mr != NULL && (memory_region_is_iommu(section.mr) || memory_region_is_ram(section.mr));
        if (section.mr != NULL) { memory_region_unref(section.mr); }
        if (!candidate) { return false; }
    }

    BQL_LOCK_GUARD();
    QEMU_LOCK_GUARD(&accel_private_lock);
    RCU_READ_LOCK_GUARD();

    if (g_hash_table_contains(accel_private_pages, &base)) { return false; }

    section = memory_region_find(get_system_memory(), base, page_size);
    if (section.mr != NULL) {
        memory_region_unref(section.mr);
        return false;
    }

    space   = accel_private_space(as);
    section = memory_region_find(as->root, base, page_size);
    if (section.mr != NULL && memory_region_is_iommu(section.mr)) {
        int           iommu_idx;
        IOMMUTLBEntry entry;

        iommu     = IOMMU_MEMORY_REGION(section.mr);
        iommu_idx = memory_region_iommu_attrs_to_index(iommu, MEMTXATTRS_UNSPECIFIED);
        iova      = section.offset_within_region;
        accel_private_watch_iommu(space, iommu, iommu_idx);
        entry = memory_region_get_iommu_class_nocheck(iommu)->translate(iommu, iova, IOMMU_NONE, iommu_idx);
        memory_region_unref(section.mr);
        section.mr = NULL;

        if ((entry.perm & IOMMU_RO) && entry.addr_mask >= page_size - 1) {
            section  = memory_region_find(entry.target_as->root, entry.translated_addr & ~(page_size - 1), page_size);
            writable = entry.perm & IOMMU_WO;
        }
    }
    if (section.mr == NULL) { return false; }

    writable = writable && !section.readonly;
    if (!memory_region_is_ram(section.mr) || int128_get64(section.size) != page_size || (is_write && !writable)) {
        memory_region_unref(section.mr);
        return false;
    }

    page        = g_new0(AccelPrivatePage, 1);
    page->ipa   = base;
    page->space = space;
    page->iommu = iommu;
    page->iova  = iova;

    accel_private_ops->map((uint8_t*)memory_region_get_ram_ptr(section.mr) + section.offset_within_region, base,
                           page_size, writable);
    memory_region_unref(section.mr);
    g_hash_table_insert(accel_private_pages, &page->ipa, page);
    return true;
}

void accel_private_system_region_add(MemoryRegionSection* section) { accel_private_drop_section(NULL, section); }

void accel_private_init(const AccelPrivateOps* ops)
{
    accel_private_ops = ops;
    qemu_mutex_init(&accel_private_lock);
    accel_private_spaces = g_ptr_array_new();
    accel_private_pages  = g_hash_table_new(g_int64_hash, g_int64_equal);
}
