/*
 * vresearch101 SEP mailbox ("iop-sep,vpiop")
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The AP and the SEP core both see this device at physical 0x30250000, each
 * through its own register bank. Layout, from AVPBooter's and
 * AVPSEPBooter's drivers (offsets per bank):
 *
 *   0x010  CTRL     bit 0 enable
 *   0x014  STATUS   bit 31 ready (set by writing it); bits 3:0 cause, W1C
 *   0x018  IRQ_EN   bit 4 inbox not empty, bit 5 outbox empty
 *   0x01c  INBOX    bit 17 empty, bit 16 full, 7:0 count
 *   0x020  OUTBOX   bit 17 empty, bit 16 full, 7:0 count
 *   0x100  inbox message (16 bytes), popped by reading its last word
 *   0x200  outbox message (16 bytes), pushed by writing its last word
 *
 * One side's outbox is the other side's inbox. The SEP waits with WFE and
 * polls; the AP gets level interrupts (inbox not empty, outbox empty).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/vmapple/vmapple.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_VPIOP TYPE_VMAPPLE_VPIOP
OBJECT_DECLARE_SIMPLE_TYPE(VPIOPState, VPIOP)

#define VPIOP_MMIO_SIZE      0x1000
#define VPIOP_FIFO_DEPTH     16
#define VPIOP_MSG_SIZE       16

#define REG_CTRL             0x010
#define REG_STATUS           0x014
#define REG_IRQ_EN           0x018
#define REG_INBOX            0x01c
#define REG_OUTBOX           0x020
#define REG_INBOX_MSG        0x100
#define REG_OUTBOX_MSG       0x200

#define STATUS_READY         BIT(31)
#define QUEUE_EMPTY          BIT(17)
#define QUEUE_FULL           BIT(16)
#define IRQ_EN_INBOX         BIT(4)
#define IRQ_EN_OUTBOX        BIT(5)

enum { SIDE_AP, SIDE_SEP, SIDE_COUNT };

typedef struct VPIOPQueue {
    uint8_t msg[VPIOP_FIFO_DEPTH][VPIOP_MSG_SIZE];
    uint32_t head;
    uint32_t count;
} VPIOPQueue;

typedef struct VPIOPSide {
    uint32_t ctrl;
    uint32_t status;
    uint32_t irq_en;
    uint8_t out_msg[VPIOP_MSG_SIZE];
    /* messages waiting for this side to read */
    VPIOPQueue inbox;
} VPIOPSide;

typedef struct VPIOPBank {
    VPIOPState *s;
    int side;
} VPIOPBank;

struct VPIOPState {
    SysBusDevice parent_obj;

    MemoryRegion iomem[SIDE_COUNT];
    VPIOPBank bank[SIDE_COUNT];
    VPIOPSide side[SIDE_COUNT];
    qemu_irq irq_inbox;
    qemu_irq irq_outbox;
};

static const char *side_name(int side)
{
    return side == SIDE_AP ? "ap" : "sep";
}

static void vpiop_update_irq(VPIOPState *s)
{
    VPIOPSide *ap = &s->side[SIDE_AP];
    VPIOPQueue *ap_out = &s->side[SIDE_SEP].inbox;

    qemu_set_irq(s->irq_inbox,
                 (ap->irq_en & IRQ_EN_INBOX) && ap->inbox.count != 0);
    qemu_set_irq(s->irq_outbox,
                 (ap->irq_en & IRQ_EN_OUTBOX) && ap_out->count == 0);
}

static uint32_t queue_status(VPIOPQueue *q)
{
    uint32_t val = q->count & 0xff;

    if (q->count == 0) {
        val |= QUEUE_EMPTY;
    }
    if (q->count == VPIOP_FIFO_DEPTH) {
        val |= QUEUE_FULL;
    }
    return val;
}

static void vpiop_push(VPIOPState *s, int from)
{
    VPIOPSide *src = &s->side[from];
    VPIOPQueue *q = &s->side[!from].inbox;

    trace_vpiop_msg(side_name(from), side_name(!from),
                    ldq_le_p(src->out_msg), ldq_le_p(src->out_msg + 8));
    if (q->count == VPIOP_FIFO_DEPTH) {
        qemu_log_mask(LOG_GUEST_ERROR, "vpiop: %s outbox overflow\n",
                      side_name(from));
        return;
    }
    memcpy(q->msg[(q->head + q->count) % VPIOP_FIFO_DEPTH], src->out_msg,
           VPIOP_MSG_SIZE);
    q->count++;
    memset(src->out_msg, 0, VPIOP_MSG_SIZE);
}

static uint64_t vpiop_read(void *opaque, hwaddr addr, unsigned size)
{
    VPIOPBank *b = opaque;
    VPIOPState *s = b->s;
    VPIOPSide *me = &s->side[b->side];
    uint64_t val = 0;

    switch (addr) {
    case REG_CTRL:
        val = me->ctrl;
        break;
    case REG_STATUS:
        val = me->status;
        break;
    case REG_IRQ_EN:
        val = me->irq_en;
        break;
    case REG_INBOX:
        val = queue_status(&me->inbox);
        break;
    case REG_OUTBOX:
        val = queue_status(&s->side[!b->side].inbox);
        break;
    case REG_INBOX_MSG ... REG_INBOX_MSG + VPIOP_MSG_SIZE - 1:
        if (me->inbox.count) {
            uint8_t *msg = me->inbox.msg[me->inbox.head];

            val = ldn_le_p(msg + (addr - REG_INBOX_MSG), size);
            /* reading the last word pops the message */
            if (addr + size == REG_INBOX_MSG + VPIOP_MSG_SIZE) {
                me->inbox.head = (me->inbox.head + 1) % VPIOP_FIFO_DEPTH;
                me->inbox.count--;
                vpiop_update_irq(s);
            }
        }
        break;
    case REG_OUTBOX_MSG ... REG_OUTBOX_MSG + VPIOP_MSG_SIZE - 1:
        val = ldn_le_p(me->out_msg + (addr - REG_OUTBOX_MSG), size);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "vpiop: %s read  0x%03" HWADDR_PRIx "\n",
                      side_name(b->side), addr);
        break;
    }
    return val;
}

static void vpiop_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VPIOPBank *b = opaque;
    VPIOPState *s = b->s;
    VPIOPSide *me = &s->side[b->side];

    switch (addr) {
    case REG_CTRL:
        me->ctrl = val;
        break;
    case REG_STATUS:
        me->status |= val & STATUS_READY;
        me->status &= ~(val & 0xf);
        break;
    case REG_IRQ_EN:
        me->irq_en = val;
        break;
    case REG_OUTBOX_MSG ... REG_OUTBOX_MSG + VPIOP_MSG_SIZE - 1:
        stn_le_p(me->out_msg + (addr - REG_OUTBOX_MSG), size, val);
        /* writing the last word sends the message */
        if (addr + size == REG_OUTBOX_MSG + VPIOP_MSG_SIZE) {
            vpiop_push(s, b->side);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "vpiop: %s write 0x%03" HWADDR_PRIx
                      " <- 0x%" PRIx64 "\n", side_name(b->side), addr, val);
        break;
    }
    vpiop_update_irq(s);
}

static const MemoryRegionOps vpiop_ops = {
    .read = vpiop_read,
    .write = vpiop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static void vpiop_reset(DeviceState *dev)
{
    VPIOPState *s = VPIOP(dev);

    memset(s->side, 0, sizeof(s->side));
    vpiop_update_irq(s);
}

static void vpiop_init(Object *obj)
{
    VPIOPState *s = VPIOP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    for (i = 0; i < SIDE_COUNT; i++) {
        s->bank[i].s = s;
        s->bank[i].side = i;
        memory_region_init_io(&s->iomem[i], obj, &vpiop_ops, &s->bank[i],
                              i == SIDE_AP ? "vpiop-ap" : "vpiop-sep",
                              VPIOP_MMIO_SIZE);
        sysbus_init_mmio(sbd, &s->iomem[i]);
    }
    sysbus_init_irq(sbd, &s->irq_inbox);
    sysbus_init_irq(sbd, &s->irq_outbox);
}

static const VMStateDescription vmstate_vpiop_queue = {
    .name = "vpiop-queue",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_2DARRAY(msg, VPIOPQueue, VPIOP_FIFO_DEPTH,
                              VPIOP_MSG_SIZE),
        VMSTATE_UINT32(head, VPIOPQueue),
        VMSTATE_UINT32(count, VPIOPQueue),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_vpiop_side = {
    .name = "vpiop-side",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, VPIOPSide),
        VMSTATE_UINT32(status, VPIOPSide),
        VMSTATE_UINT32(irq_en, VPIOPSide),
        VMSTATE_UINT8_ARRAY(out_msg, VPIOPSide, VPIOP_MSG_SIZE),
        VMSTATE_STRUCT(inbox, VPIOPSide, 1, vmstate_vpiop_queue, VPIOPQueue),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_vpiop = {
    .name = TYPE_VMAPPLE_VPIOP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(side, VPIOPState, SIDE_COUNT, 1,
                             vmstate_vpiop_side, VPIOPSide),
        VMSTATE_END_OF_LIST()
    }
};

static void vpiop_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, vpiop_reset);
    dc->vmsd = &vmstate_vpiop;
    dc->desc = "vresearch101 SEP mailbox";
}

static const TypeInfo vpiop_info = {
    .name          = TYPE_VMAPPLE_VPIOP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VPIOPState),
    .instance_init = vpiop_init,
    .class_init    = vpiop_class_init,
};

static void vpiop_register_types(void)
{
    type_register_static(&vpiop_info);
}

type_init(vpiop_register_types)
