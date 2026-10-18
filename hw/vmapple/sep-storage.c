/*
 * vresearch101 SEP storage ("VSTR")
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Virtualization.framework gives the virtual SEP a small persistent disk
 * (the VM bundle's SEPStorage file). sepOS's SEPD reaches it through a
 * register window (its resource table's VSTR object, SEP physical
 * 0x30020000) and a 16 KiB request buffer (VSTD, 0x8000000). The layout
 * below is read from SEPD's driver, not from documentation:
 *
 *   0x000  ID        reads 1
 *   0x004  TYPE      reads 2 (the virtio block device ID)
 *   0x008  CFG       offset of a virtio_blk_config (64-bit read)
 *   0x010  DONE      non-zero once the last request has completed
 *   0x400  ENABLE    written 1, read back
 *   0x404  STATUS    driver status (written 3)
 *   0x408  NOTIFY    64-bit write: physical address of a descriptor chain
 *
 * SEPD keeps its request in VSTD: a virtio_blk_outhdr at +0, the status
 * byte at +0x20, the descriptor chain at +0x40 and data from +0x1000.
 * Descriptors are 16 bytes: addr (64), len (32), a 16-bit word SEPD sets to 1, then flags
 * (bit 0 next, bit 1 device-writable). Requests complete synchronously.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/vmapple/vmapple.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(SEPStorageState, VMAPPLE_SEP_STORAGE)

#define SEP_STORAGE_MMIO_SIZE   0x4000
#define SEP_STORAGE_SECTOR      512

#define REG_ID                  0x000
#define REG_TYPE                0x004
#define REG_CFG                 0x008
#define REG_DONE                0x010
#define REG_CFG_BASE            0x100   /* virtio_blk_config lives here */
#define REG_CFG_END             (REG_CFG_BASE + 0x40)
#define REG_ENABLE              0x400
#define REG_STATUS              0x404
#define REG_NOTIFY              0x408

#define REQ_MAX_DESC            16
#define DESC_F_NEXT             1
#define DESC_F_WRITE            2

#define BLK_T_IN                0
#define BLK_T_OUT               1
#define BLK_T_FLUSH             4
#define BLK_S_OK                0
#define BLK_S_IOERR             1
#define BLK_S_UNSUPP            2

struct SEPStorageState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    BlockBackend *blk;

    uint32_t enable;
    uint32_t status;
    uint32_t done;
    uint64_t notify;
};

typedef struct SEPStorageDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
} SEPStorageDesc;

static uint64_t sep_storage_sectors(SEPStorageState *s)
{
    int64_t len = s->blk ? blk_getlength(s->blk) : 0;

    return len > 0 ? len / SEP_STORAGE_SECTOR : 0;
}

static uint8_t sep_storage_rw(SEPStorageState *s, bool write, uint64_t sector,
                              SEPStorageDesc *d, int n)
{
    uint64_t off = sector * SEP_STORAGE_SECTOR;
    uint64_t end = sep_storage_sectors(s) * SEP_STORAGE_SECTOR;

    for (int i = 0; i < n; i++) {
        g_autofree uint8_t *buf = g_malloc(d[i].len);

        if (off + d[i].len > end) {
            qemu_log_mask(LOG_GUEST_ERROR, "sep-storage: %s past the end "
                          "(off 0x%" PRIx64 " len 0x%x)\n",
                          write ? "write" : "read", off, d[i].len);
            return BLK_S_IOERR;
        }
        if (write) {
            if (dma_memory_read(&s->dma_as, d[i].addr, buf, d[i].len,
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
                blk_pwrite(s->blk, off, d[i].len, buf, 0) < 0) {
                return BLK_S_IOERR;
            }
        } else {
            if (blk_pread(s->blk, off, d[i].len, buf, 0) < 0 ||
                dma_memory_write(&s->dma_as, d[i].addr, buf, d[i].len,
                                 MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                return BLK_S_IOERR;
            }
        }
        off += d[i].len;
    }
    return BLK_S_OK;
}

static void sep_storage_request(SEPStorageState *s, hwaddr desc_table)
{
    SEPStorageDesc d[REQ_MAX_DESC];
    uint8_t raw[16], hdr[16];
    uint8_t status;
    uint32_t type;
    uint64_t sector;
    int n = 0;

    /* Walk the chain: header, data..., status */
    do {
        if (n == REQ_MAX_DESC ||
            dma_memory_read(&s->dma_as, desc_table + n * 16, raw,
                            sizeof(raw), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sep-storage: bad descriptor chain at 0x%" HWADDR_PRIx
                          "\n", desc_table);
            return;
        }
        d[n].addr = ldq_le_p(raw);
        d[n].len = ldl_le_p(raw + 8);
        d[n].flags = lduw_le_p(raw + 14);
        n++;
    } while (d[n - 1].flags & DESC_F_NEXT);

    if (n < 2 || d[0].len < sizeof(hdr) || d[n - 1].len < 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "sep-storage: short request (%d)\n", n);
        return;
    }
    dma_memory_read(&s->dma_as, d[0].addr, hdr, sizeof(hdr),
                    MEMTXATTRS_UNSPECIFIED);
    type = ldl_le_p(hdr);
    sector = ldq_le_p(hdr + 8);

    switch (type) {
    case BLK_T_IN:
    case BLK_T_OUT:
        status = s->blk ? sep_storage_rw(s, type == BLK_T_OUT, sector,
                                         &d[1], n - 2)
                        : BLK_S_IOERR;
        break;
    case BLK_T_FLUSH:
        status = s->blk && blk_flush(s->blk) < 0 ? BLK_S_IOERR : BLK_S_OK;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sep-storage: request type %u\n", type);
        status = BLK_S_UNSUPP;
        break;
    }
    trace_sep_storage_request(type, sector, n, status);
    dma_memory_write(&s->dma_as, d[n - 1].addr, &status, 1,
                     MEMTXATTRS_UNSPECIFIED);
}

static uint64_t sep_storage_read(void *opaque, hwaddr addr, unsigned size)
{
    SEPStorageState *s = opaque;
    uint8_t cfg[REG_CFG_END - REG_CFG_BASE] = { 0 };

    switch (addr) {
    case REG_ID:
        return 1;
    case REG_TYPE:
        return 2;
    case REG_CFG:
        return REG_CFG_BASE;
    case REG_DONE:
        return s->done;
    case REG_ENABLE:
        return s->enable;
    case REG_STATUS:
        return s->status;
    case REG_CFG_BASE ... REG_CFG_END - 1:
        stq_le_p(cfg, sep_storage_sectors(s));          /* capacity */
        stl_le_p(cfg + 0x14, SEP_STORAGE_SECTOR);       /* blk_size */
        if (addr + size > REG_CFG_END) {
            return 0;
        }
        return ldn_le_p(cfg + (addr - REG_CFG_BASE), size);
    default:
        qemu_log_mask(LOG_UNIMP, "sep-storage: read 0x%03" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void sep_storage_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    SEPStorageState *s = opaque;

    switch (addr) {
    case REG_ENABLE:
        s->enable = val;
        break;
    case REG_STATUS:
        s->status = val;
        break;
    case REG_NOTIFY:
        s->notify = size == 8 ? val : deposit64(s->notify, 0, 32, val);
        if (size == 8) {
            s->done = 0;
            sep_storage_request(s, s->notify);
            s->done = 1;
        }
        break;
    case REG_NOTIFY + 4:
        s->notify = deposit64(s->notify, 32, 32, val);
        s->done = 0;
        sep_storage_request(s, s->notify);
        s->done = 1;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sep-storage: write 0x%03" HWADDR_PRIx
                      " <- 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps sep_storage_ops = {
    .read = sep_storage_read,
    .write = sep_storage_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static void sep_storage_realize(DeviceState *dev, Error **errp)
{
    SEPStorageState *s = VMAPPLE_SEP_STORAGE(dev);

    if (!s->dma_mr) {
        error_setg(errp, "sep-storage: 'dma-memory' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "sep-storage-dma");
    if (s->blk) {
        if (!blk_supports_write_perm(s->blk)) {
            error_setg(errp, "sep-storage: drive is read-only");
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
    }
    memory_region_init_io(&s->iomem, OBJECT(dev), &sep_storage_ops, s,
                          "sep-storage", SEP_STORAGE_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void sep_storage_reset(DeviceState *dev)
{
    SEPStorageState *s = VMAPPLE_SEP_STORAGE(dev);

    s->enable = s->status = s->done = 0;
    s->notify = 0;
}

static const Property sep_storage_properties[] = {
    DEFINE_PROP_DRIVE("drive", SEPStorageState, blk),
    DEFINE_PROP_LINK("dma-memory", SEPStorageState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const VMStateDescription vmstate_sep_storage = {
    .name = TYPE_VMAPPLE_SEP_STORAGE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(enable, SEPStorageState),
        VMSTATE_UINT32(status, SEPStorageState),
        VMSTATE_UINT32(done, SEPStorageState),
        VMSTATE_UINT64(notify, SEPStorageState),
        VMSTATE_END_OF_LIST()
    }
};

static void sep_storage_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sep_storage_realize;
    device_class_set_legacy_reset(dc, sep_storage_reset);
    device_class_set_props(dc, sep_storage_properties);
    dc->vmsd = &vmstate_sep_storage;
    dc->desc = "vresearch101 SEP storage";
}

static const TypeInfo sep_storage_info = {
    .name          = TYPE_VMAPPLE_SEP_STORAGE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SEPStorageState),
    .class_init    = sep_storage_class_init,
};

static void sep_storage_register_types(void)
{
    type_register_static(&sep_storage_info);
}

type_init(sep_storage_register_types)
