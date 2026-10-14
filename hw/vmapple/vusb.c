/*
 * vresearch101 virtual USB device controller ("virtio-usb", AVP device 0x1a01)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AVPBooter's DFU mode (and iBSS/iBEC after it) present "Apple Mobile Device
 * (DFU Mode)" through this device, which sits in the backdoor interface's
 * USB slot (0x30100000) and uses the same paravirtual transport as the
 * bdif block devices:
 *
 *   0x000  status (1: present)
 *   0x004  device id (0x1a01)
 *   0x008  offset of the queue registers (64-bit)
 *   0x400  reset handshake (written 1, read back 1)
 *   0x404  driver status
 *
 * First cut: a register store that answers the probe and logs everything
 * else, to learn the queue protocol from the guest's driver.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/vmapple/vmapple.h"
#include "trace.h"
#include "system/dma.h"
#include "qemu/cutils.h"

#define TYPE_VUSB TYPE_VMAPPLE_VUSB
OBJECT_DECLARE_SIMPLE_TYPE(VUSBState, VUSB)

#define VUSB_MMIO_SIZE      0x10000
#define VUSB_DEVICE_ID      0x1a01
#define VUSB_QUEUE_REGS     0x420

#define REG_STATUS          0x000
#define REG_DEVICE_ID       0x004
#define REG_QUEUE_REGS      0x008
#define REG_BUSY            0x010   /* bit 0: ready, as in bdif */
#define REG_RESET           0x400
#define REG_QUEUE_ADDR      0x408   /* 64-bit guest address */
#define REG_QUEUE_SEL       0x410

struct VUSBState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint8_t regs[VUSB_MMIO_SIZE];
};

static uint64_t vusb_read(void *opaque, hwaddr addr, unsigned size)
{
    VUSBState *s = opaque;
    uint64_t val;

    switch (addr) {
    case REG_STATUS:
        val = 1;
        break;
    case REG_DEVICE_ID:
        val = VUSB_DEVICE_ID;
        break;
    case REG_QUEUE_REGS:
        val = VUSB_QUEUE_REGS;
        break;
    case REG_BUSY:
        val = 1;
        break;
    default:
        val = ldn_le_p(&s->regs[addr], size);
        break;
    }
    trace_vusb_read(addr, size, val);
    return val;
}

static void vusb_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VUSBState *s = opaque;

    trace_vusb_write(addr, size, val);
    stn_le_p(&s->regs[addr], size, val);

    if (addr == REG_QUEUE_ADDR) {
        /* learning the protocol: show what the address points to */
        uint8_t buf[0x80];
        g_autoptr(GString) hex = g_string_new("");

        dma_memory_read(&address_space_memory, val, buf, sizeof(buf),
                        MEMTXATTRS_UNSPECIFIED);
        for (int i = 0; i < sizeof(buf); i += 8) {
            g_string_append_printf(hex, "%s%016" PRIx64, i ? " " : "",
                                   ldq_le_p(&buf[i]));
        }
        qemu_log_mask(LOG_UNIMP, "vusb: queue %u at 0x%" PRIx64 ": %s\n",
                      ldl_le_p(&s->regs[REG_QUEUE_SEL]), val, hex->str);
    }
}

static const MemoryRegionOps vusb_ops = {
    .read = vusb_read,
    .write = vusb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void vusb_reset(DeviceState *dev)
{
    VUSBState *s = VUSB(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void vusb_init(Object *obj)
{
    VUSBState *s = VUSB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &vusb_ops, s, "vresearch-vusb",
                          VUSB_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void vusb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, vusb_reset);
    dc->desc = "vresearch101 virtual USB device controller";
}

static const TypeInfo vusb_info = {
    .name          = TYPE_VMAPPLE_VUSB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VUSBState),
    .instance_init = vusb_init,
    .class_init    = vusb_class_init,
};

static void vusb_register_types(void)
{
    type_register_static(&vusb_info);
}

type_init(vusb_register_types)
