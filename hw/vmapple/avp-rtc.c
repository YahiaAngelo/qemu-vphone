/*
 * vresearch101 real-time clock ("avp,rtc")
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Layout, from XNU's AppleVirtualPlatformRTC (AppleVirtualPlatform.kext,
 * iOS 27.1 vphone600 kernelcache); all registers are 64 bits wide:
 *
 *   0x00  COUNTER     microseconds, read-only, free running
 *   0x18  CTRL        bit 0 set by the driver (start, and when it stops)
 *   0x20  IRQ_EN      bits 1 and 2: the driver writes 6, and 0 when it stops
 *   0x30  IRQ_STATUS  read by the interrupt handler
 *   0x38  IRQ_CLEAR   write 1 to clear IRQ_STATUS bits
 *   0x40  ID          32 bits, read-only: 0xf001
 *
 * iBoot reads ID while it builds the device tree: 0xf001 keeps "avp-rtc" and
 * deletes "pl031-rtc"; any other value deletes "avp-rtc", and XNU then takes
 * its time from the PL031 (ApplePL031RTC), whose time of day set by the
 * guest lasts only until QEMU exits. The id property can select that.
 *
 * The driver has no register for the time of day. It keeps an offset in
 * microseconds, persisted in the NVRAM variable
 * "com.apple.System.rtc-offset" ("0x%llx"; missing on a first boot, then
 * 0): getGMTTimeOfDay returns (COUNTER + offset) / 1000000, and
 * setGMTTimeOfDay stores seconds * 1000000 - COUNTER as the new offset and
 * writes it to NVRAM. COUNTER must therefore keep running, across power
 * cycles too, as the hardware counter behind a PMU RTC does. Here it is the
 * RTC's wall-clock time (-rtc base=, clock=; UTC host time by default) in
 * microseconds since 1970, so a guest with no offset (a first boot, or one
 * that never set its clock) has the right time, and one that set its clock
 * by hand keeps the same difference from it on every boot.
 *
 * The handler turns IRQ_STATUS bit 2 into message 1 and bit 1 into
 * message 0 (kIOMessageDeviceHasPoweredOn) to the driver's clients. What
 * raises them is not known, so this model never sets them.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/vmapple/vmapple.h"
#include "migration/vmstate.h"
#include "system/rtc.h"
#include "system/system.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(AVPRTCState, VMAPPLE_AVP_RTC)

#define AVP_RTC_MMIO_SIZE  0x1000

#define REG_COUNTER        0x00
#define REG_CTRL           0x18
#define REG_IRQ_EN         0x20
#define REG_IRQ_STATUS     0x30
#define REG_IRQ_CLEAR      0x38
#define REG_ID             0x40

#define AVP_RTC_ID         0xf001

#define IRQ_MASK           0x6

struct AVPRTCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t id;

    /* COUNTER = rtc_clock in microseconds + offset_us */
    int64_t offset_us;
    /* the high half for a 32-bit read of COUNTER + 4 after one of + 0 */
    uint32_t counter_hi;
    uint64_t ctrl;
    uint64_t irq_en;
    uint64_t irq_status;
    /* registers the driver does not use, kept as written */
    uint8_t other[AVP_RTC_MMIO_SIZE];
};

static uint64_t avp_rtc_counter(AVPRTCState *s)
{
    return qemu_clock_get_us(rtc_clock) + s->offset_us;
}

static void avp_rtc_update_irq(AVPRTCState *s)
{
    qemu_set_irq(s->irq, !!(s->irq_status & s->irq_en & IRQ_MASK));
}

static bool avp_rtc_reg(AVPRTCState *s, hwaddr reg, uint64_t **p)
{
    switch (reg) {
    case REG_CTRL:
        *p = &s->ctrl;
        return true;
    case REG_IRQ_EN:
        *p = &s->irq_en;
        return true;
    case REG_IRQ_STATUS:
        *p = &s->irq_status;
        return true;
    default:
        return false;
    }
}

static uint64_t avp_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    AVPRTCState *s = opaque;
    hwaddr reg = addr & ~7ULL;
    unsigned shift = (addr & 7) * 8;
    uint64_t *p, val;

    if (reg == REG_COUNTER) {
        if (addr == REG_COUNTER + 4 && size == 4) {
            val = s->counter_hi;
        } else {
            uint64_t now = avp_rtc_counter(s);

            s->counter_hi = now >> 32;
            val = extract64(now, shift, size * 8);
        }
    } else if (avp_rtc_reg(s, reg, &p)) {
        val = extract64(*p, shift, size * 8);
    } else if (reg == REG_ID) {
        val = extract64(s->id, shift, size * 8);
    } else {
        val = ldn_le_p(&s->other[addr], size);
        qemu_log_mask(LOG_UNIMP, "avp-rtc: read%u 0x%03" HWADDR_PRIx
                      " -> 0x%" PRIx64 " (unmodeled)\n", size * 8, addr, val);
    }
    trace_avp_rtc_read(addr, size, val);
    return val;
}

static void avp_rtc_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    AVPRTCState *s = opaque;
    hwaddr reg = addr & ~7ULL;
    unsigned shift = (addr & 7) * 8;
    uint64_t *p;

    trace_avp_rtc_write(addr, size, val);
    switch (reg) {
    case REG_COUNTER:
        qemu_log_mask(LOG_GUEST_ERROR, "avp-rtc: write to read-only COUNTER"
                      " (0x%03" HWADDR_PRIx " <- 0x%" PRIx64 ")\n", addr, val);
        return;
    case REG_IRQ_STATUS:
    case REG_ID:
        qemu_log_mask(LOG_GUEST_ERROR, "avp-rtc: write to read-only register"
                      " (0x%03" HWADDR_PRIx " <- 0x%" PRIx64 ")\n", addr, val);
        return;
    case REG_IRQ_CLEAR:
        s->irq_status &= ~(val << shift);
        avp_rtc_update_irq(s);
        return;
    }
    if (avp_rtc_reg(s, reg, &p)) {
        *p = deposit64(*p, shift, size * 8, val);
        avp_rtc_update_irq(s);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "avp-rtc: write%u 0x%03" HWADDR_PRIx
                  " <- 0x%" PRIx64 " (unmodeled)\n", size * 8, addr, val);
    stn_le_p(&s->other[addr], size, val);
}

static const MemoryRegionOps avp_rtc_ops = {
    .read = avp_rtc_read,
    .write = avp_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

/*
 * offset_us such that COUNTER is the RTC's time of day: the seconds come
 * from qemu_get_timedate() (which applies -rtc base=), the microseconds
 * from rtc_clock, read in the same second.
 */
static int64_t avp_rtc_wallclock_offset(void)
{
    struct tm tm;
    int64_t before, after;

    do {
        before = qemu_clock_get_ms(rtc_clock) / 1000;
        qemu_get_timedate(&tm, 0);
        after = qemu_clock_get_ms(rtc_clock) / 1000;
    } while (before != after);
    return (mktimegm(&tm) - before) * G_USEC_PER_SEC;
}

static void avp_rtc_realize(DeviceState *dev, Error **errp)
{
    AVPRTCState *s = VMAPPLE_AVP_RTC(dev);

    s->offset_us = avp_rtc_wallclock_offset();
}

static void avp_rtc_reset_hold(Object *obj, ResetType type)
{
    AVPRTCState *s = VMAPPLE_AVP_RTC(obj);

    /* the counter is always on; only the driver's registers reset */
    s->ctrl = 0;
    s->irq_en = 0;
    s->irq_status = 0;
    s->counter_hi = 0;
    memset(s->other, 0, sizeof(s->other));
    avp_rtc_update_irq(s);
}

static void avp_rtc_init(Object *obj)
{
    AVPRTCState *s = VMAPPLE_AVP_RTC(obj);

    memory_region_init_io(&s->iomem, obj, &avp_rtc_ops, s, "avp-rtc",
                          AVP_RTC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

/*
 * offset_us is relative to rtc_clock, so it migrates as is: with
 * clock=host COUNTER stays the host's time of day, with clock=vm it
 * follows the migrated virtual clock.
 */
static const VMStateDescription vmstate_avp_rtc = {
    .name = TYPE_VMAPPLE_AVP_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(offset_us, AVPRTCState),
        VMSTATE_UINT32(counter_hi, AVPRTCState),
        VMSTATE_UINT64(ctrl, AVPRTCState),
        VMSTATE_UINT64(irq_en, AVPRTCState),
        VMSTATE_UINT64(irq_status, AVPRTCState),
        VMSTATE_UINT8_ARRAY(other, AVPRTCState, AVP_RTC_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static const Property avp_rtc_properties[] = {
    DEFINE_PROP_UINT32("id", AVPRTCState, id, AVP_RTC_ID),
};

static void avp_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = avp_rtc_realize;
    dc->vmsd = &vmstate_avp_rtc;
    device_class_set_props(dc, avp_rtc_properties);
    dc->desc = "vresearch101 AVP real-time clock";
    rc->phases.hold = avp_rtc_reset_hold;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo avp_rtc_info = {
    .name          = TYPE_VMAPPLE_AVP_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AVPRTCState),
    .instance_init = avp_rtc_init,
    .class_init    = avp_rtc_class_init,
};

static void avp_rtc_register_types(void)
{
    type_register_static(&avp_rtc_info);
}

type_init(avp_rtc_register_types)
