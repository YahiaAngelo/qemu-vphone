/*
 * vresearch101 virtual USB device controller (AVP device 0x1a01)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AVPBooter's DFU mode and iBoot's recovery mode present the device side of
 * "Apple Mobile Device" through this controller, which sits in the backdoor
 * interface's USB slot (0x30100000) and uses the same paravirtual transport
 * as the bdif block devices:
 *
 *   0x000  status (1: present)
 *   0x004  device id (0x1a01)
 *   0x008  offset of the queue registers (64-bit)
 *   0x010  completion of the selected queue's request (bit 0)
 *   0x400  reset handshake (written 1, read back 1)
 *   0x404  driver status
 *   0x408  guest address of a request for the selected queue (64-bit)
 *   0x410  queue select
 *
 * A request is a chain of up to VUSB_CHAIN_MAX descriptors
 * { u64 addr; u32 len; u16 count (1); u16 flags }, flags bit 0 meaning
 * another descriptor follows and bit 1 that the device writes the buffer.
 * Queue 0 receives messages from the USB host, queue 1 sends messages to
 * it. The driver polls 0x10: a TX request completes at once, an RX request
 * when the host has a message for it. Posting a request clears the queue's
 * completion; the TX queue starts out complete (iBoot waits for an idle TX
 * queue before each send and panics after 5 s). On completion the device
 * also writes the request's first descriptor back: flags = 0x8000 and, for
 * a receive, len = the bytes written. OpenAVB polls that word in RAM instead
 * of 0x10 (Virtualization.framework does both); AVPBooter and iBoot ignore it.
 *
 * Host to device message: { u32 len; u8 ep; u8 type; u8 payload[len] }
 *   type 1: data for an endpoint; on EP0 OUT (ep 0x00) the payload starts
 *           with the 8-byte SETUP packet, followed by any OUT data stage;
 *           on an IN endpoint it asks for the next IN transfer
 *   type 2: bus reset (iBoot's USB core event 2)
 *   type 3: endpoint abort
 * Device to host message: { u8 code; u8 ep (EP0 IN as 0); u8 data[] }
 *   code 1: data on an IN endpoint / OUT transfer complete
 *   code 2: stall (?)
 *   code 4: bus reset done
 *   code 5: abort done
 *   code 6: driver reset: the guest (re)initialised the controller, as each
 *           boot stage does (AVPBooter's DFU, iBoot, the restore OS). The
 *           USB device it presented is gone; the host should treat this as
 *           an unplug and enumerate the new device. Sent with ep 0, no data.
 *
 * Inferred from AVPBooter's and iBoot's drivers. With a chardev attached,
 * the host side is a byte stream: host to device messages are sent as they
 * are (they carry their length), device to host messages are prefixed with
 * a u32 little-endian length. The test-enum property makes the device send
 * itself a bus reset and GET_DESCRIPTOR(device) once the driver posts its
 * first receive buffer (iBoot's recovery mode answers with the 05ac:1281
 * device descriptor).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "hw/vmapple/vmapple.h"
#include "system/dma.h"
#include "trace.h"

#define TYPE_VUSB TYPE_VMAPPLE_VUSB
OBJECT_DECLARE_SIMPLE_TYPE(VUSBState, VUSB)

#define VUSB_MMIO_SIZE      0x10000
#define VUSB_DEVICE_ID      0x1a01
#define VUSB_QUEUE_REGS     0x420
#define VUSB_QUEUES         2
#define VUSB_RX             0
#define VUSB_TX             1
#define VUSB_CHAIN_MAX      8
#define VUSB_MSG_HDR        6
#define VUSB_MSG_MAX        0x10000
#define VUSB_HOST_FIFO      64
#define VUSB_CODE_DRIVER_RESET 6

#define REG_STATUS          0x000
#define REG_DEVICE_ID       0x004
#define REG_QUEUE_REGS      0x008
#define REG_COMPLETION      0x010
#define REG_RESET           0x400
#define REG_DRIVER_STATUS   0x404
#define REG_REQUEST         0x408
#define REG_QUEUE_SEL       0x410

#define DESC_F_NEXT         (1 << 0)
#define DESC_F_WRITE        (1 << 1)
/* written into a request's first descriptor when the device is done with it */
#define DESC_F_COMPLETE     0x8000

typedef struct VUSBDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
} VUSBDesc;

typedef struct VUSBQueue {
    uint64_t request;       /* guest address of the pending chain, 0: none */
    bool done;
} VUSBQueue;

struct VUSBState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    CharBackend chr;
    bool test_enum;
    bool test_sent;

    uint32_t reset;
    uint32_t driver_status;
    uint32_t queue_sel;
    VUSBQueue queues[VUSB_QUEUES];

    /* messages from the host waiting for a receive buffer */
    GQueue host_msgs;
    /* the VirtIO-PCI transport, and whether its driver owns the link */
    VUSBAltTransport alt;
    bool alt_active;
    /* partial message from the chardev */
    GByteArray *rx_buf;
};

static int vusb_read_chain(VUSBState *s, uint64_t addr, VUSBDesc *descs)
{
    int n = 0;
    uint8_t raw[16];

    do {
        if (n == VUSB_CHAIN_MAX ||
            dma_memory_read(&address_space_memory, addr + n * 16, raw,
                            sizeof(raw), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vusb: bad request chain at 0x%" PRIx64 "\n", addr);
            return -1;
        }
        descs[n].addr = ldq_le_p(&raw[0]);
        descs[n].len = ldl_le_p(&raw[8]);
        descs[n].flags = lduw_le_p(&raw[14]);
    } while (descs[n++].flags & DESC_F_NEXT);

    return n;
}

static void vusb_send_to_host(VUSBState *s, const uint8_t *msg, uint32_t len)
{
    uint8_t hdr[4];

    trace_vusb_tx(len, len > 0 ? msg[0] : 0, len > 1 ? msg[1] : 0);
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }
    stl_le_p(hdr, len);
    qemu_chr_fe_write_all(&s->chr, hdr, sizeof(hdr));
    qemu_chr_fe_write_all(&s->chr, msg, len);
}

/*
 * The guest's driver took the controller over afresh: whatever the previous
 * stage presented is unplugged. Host messages queued for it are dropped (the
 * new driver must not see them) and the host is told to enumerate again.
 */
static void vusb_driver_reset(VUSBState *s)
{
    static const uint8_t detach[2] = { VUSB_CODE_DRIVER_RESET, 0 };
    GByteArray *msg;

    while ((msg = g_queue_pop_head(&s->host_msgs))) {
        g_byte_array_unref(msg);
    }
    vusb_send_to_host(s, detach, sizeof(detach));
}

/* mark a request done in its first descriptor (len only when used >= 0) */
static void vusb_write_back(uint64_t request, int64_t used)
{
    uint8_t len[4];
    uint8_t flags[2];

    if (used >= 0) {
        stl_le_p(len, used);
        dma_memory_write(&address_space_memory, request + 8, len, sizeof(len),
                         MEMTXATTRS_UNSPECIFIED);
    }
    stw_le_p(flags, DESC_F_COMPLETE);
    dma_memory_write(&address_space_memory, request + 14, flags,
                     sizeof(flags), MEMTXATTRS_UNSPECIFIED);
}

/* gather a transmit request and pass it to the host */
static void vusb_do_tx(VUSBState *s, VUSBQueue *q)
{
    VUSBDesc descs[VUSB_CHAIN_MAX];
    g_autoptr(GByteArray) msg = g_byte_array_new();
    int n = vusb_read_chain(s, q->request, descs);

    for (int i = 0; i < n; i++) {
        uint32_t off = msg->len;

        if (descs[i].flags & DESC_F_WRITE ||
            off + descs[i].len > VUSB_MSG_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR, "vusb: bad transmit descriptor\n");
            break;
        }
        g_byte_array_set_size(msg, off + descs[i].len);
        dma_memory_read(&address_space_memory, descs[i].addr,
                        msg->data + off, descs[i].len,
                        MEMTXATTRS_UNSPECIFIED);
    }
    /* OpenAVB answers a malformed event with an empty send: nothing to pass on */
    if (n > 0 && msg->len > 0) {
        vusb_send_to_host(s, msg->data, msg->len);
    }
    if (n > 0) {
        vusb_write_back(q->request, -1);
    }
    q->request = 0;
    q->done = true;
}

/* hand the oldest host message to a waiting receive request */
static void vusb_do_rx(VUSBState *s)
{
    VUSBQueue *q = &s->queues[VUSB_RX];
    VUSBDesc descs[VUSB_CHAIN_MAX];
    g_autoptr(GByteArray) msg = NULL;
    uint32_t off = 0;
    int n;

    if (!q->request || g_queue_is_empty(&s->host_msgs)) {
        return;
    }
    msg = g_queue_pop_head(&s->host_msgs);
    n = vusb_read_chain(s, q->request, descs);
    for (int i = 0; i < n && off < msg->len; i++) {
        uint32_t len = MIN(descs[i].len, msg->len - off);

        if (!(descs[i].flags & DESC_F_WRITE)) {
            qemu_log_mask(LOG_GUEST_ERROR, "vusb: bad receive descriptor\n");
            break;
        }
        dma_memory_write(&address_space_memory, descs[i].addr,
                         msg->data + off, len, MEMTXATTRS_UNSPECIFIED);
        off += len;
    }
    if (off < msg->len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vusb: %u-byte message truncated to %u\n",
                      msg->len, off);
    }
    trace_vusb_rx(msg->len, ldl_le_p(msg->data), msg->data[4], msg->data[5]);
    if (n > 0) {
        vusb_write_back(q->request, off);
    }
    q->request = 0;
    q->done = true;
}

static void vusb_queue_host_msg(VUSBState *s, GByteArray *msg)
{
    if (g_queue_get_length(&s->host_msgs) >= VUSB_HOST_FIFO) {
        qemu_log_mask(LOG_GUEST_ERROR, "vusb: host message dropped\n");
        g_byte_array_unref(msg);
        return;
    }
    g_queue_push_tail(&s->host_msgs, msg);
    if (s->alt_active) {
        if (s->alt.kick) {
            s->alt.kick(s->alt.opaque);
        }
    } else {
        vusb_do_rx(s);
    }
}

static void vusb_queue_test_msg(VUSBState *s, uint8_t ep, uint8_t type,
                                const uint8_t *payload, uint32_t len)
{
    GByteArray *msg = g_byte_array_sized_new(VUSB_MSG_HDR + len);
    uint8_t hdr[VUSB_MSG_HDR] = { 0, 0, 0, 0, ep, type };

    stl_le_p(hdr, len);
    g_byte_array_append(msg, hdr, sizeof(hdr));
    g_byte_array_append(msg, payload, len);
    vusb_queue_host_msg(s, msg);
}

static void vusb_test_enum(VUSBState *s)
{
    /* GET_DESCRIPTOR(DEVICE), 18 bytes */
    static const uint8_t setup[] = { 0x80, 0x06, 0x00, 0x01,
                                     0x00, 0x00, 0x12, 0x00 };

    vusb_queue_test_msg(s, 0x00, 2, NULL, 0);
    vusb_queue_test_msg(s, 0x00, 1, setup, sizeof(setup));
}

static uint64_t vusb_read(void *opaque, hwaddr addr, unsigned size)
{
    VUSBState *s = opaque;
    uint64_t val = 0;

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
    case REG_COMPLETION:
        val = s->queue_sel < VUSB_QUEUES && s->queues[s->queue_sel].done;
        break;
    case REG_RESET:
        val = s->reset;
        break;
    case REG_DRIVER_STATUS:
        val = s->driver_status;
        break;
    case REG_QUEUE_SEL:
        val = s->queue_sel;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "vusb: read 0x%" HWADDR_PRIx "\n", addr);
        break;
    }
    if (addr != REG_COMPLETION) {
        trace_vusb_read(addr, size, val);
    }
    return val;
}

static void vusb_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VUSBState *s = opaque;
    VUSBQueue *q;

    trace_vusb_write(addr, size, val);

    switch (addr) {
    case REG_RESET:
        s->reset = val;
        if (val) {
            /* the backdoor slot's driver (AVPBooter, iBoot) owns the link */
            s->alt_active = false;
            vusb_driver_reset(s);
        }
        break;
    case REG_DRIVER_STATUS:
        s->driver_status = val;
        break;
    case REG_QUEUE_SEL:
        s->queue_sel = val;
        break;
    case REG_REQUEST:
        if (s->queue_sel >= VUSB_QUEUES) {
            qemu_log_mask(LOG_GUEST_ERROR, "vusb: request on queue %u\n",
                          s->queue_sel);
            break;
        }
        q = &s->queues[s->queue_sel];
        q->request = val;
        q->done = false;
        if (s->queue_sel == VUSB_TX) {
            vusb_do_tx(s, q);
            break;
        }
        if (s->test_enum && !s->test_sent) {
            s->test_sent = true;
            vusb_test_enum(s);
        }
        vusb_do_rx(s);
        break;
    case VUSB_QUEUE_REGS:
        /* written 1 once during the probe */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "vusb: write 0x%" HWADDR_PRIx
                      " <- 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps vusb_ops = {
    .read = vusb_read,
    .write = vusb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static int vusb_chr_can_receive(void *opaque)
{
    return VUSB_MSG_MAX;
}

static void vusb_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    VUSBState *s = opaque;

    g_byte_array_append(s->rx_buf, buf, size);
    while (s->rx_buf->len >= VUSB_MSG_HDR) {
        uint32_t total = VUSB_MSG_HDR + ldl_le_p(s->rx_buf->data);
        GByteArray *msg;

        if (total > VUSB_MSG_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vusb: bad host message length, resyncing\n");
            g_byte_array_set_size(s->rx_buf, 0);
            return;
        }
        if (s->rx_buf->len < total) {
            return;
        }
        msg = g_byte_array_sized_new(total);
        g_byte_array_append(msg, s->rx_buf->data, total);
        g_byte_array_remove_range(s->rx_buf, 0, total);
        vusb_queue_host_msg(s, msg);
    }
}

void vusb_set_alt_transport(DeviceState *dev, const VUSBAltTransport *alt)
{
    VUSB(dev)->alt = *alt;
}

void vusb_alt_activate(DeviceState *dev, bool active)
{
    VUSBState *s = VUSB(dev);

    if (active == s->alt_active) {
        return;
    }
    s->alt_active = active;
    if (active) {
        /* a new driver, a new device: unplug whatever the last stage showed */
        vusb_driver_reset(s);
    }
}

GByteArray *vusb_pop_host_msg(DeviceState *dev)
{
    return g_queue_pop_head(&VUSB(dev)->host_msgs);
}

void vusb_unpop_host_msg(DeviceState *dev, GByteArray *msg)
{
    g_queue_push_head(&VUSB(dev)->host_msgs, msg);
}

void vusb_msg_to_host(DeviceState *dev, const uint8_t *msg, uint32_t len)
{
    vusb_send_to_host(VUSB(dev), msg, len);
}

static void vusb_reset(DeviceState *dev)
{
    VUSBState *s = VUSB(dev);
    GByteArray *msg;

    s->alt_active = false;
    s->reset = 0;
    s->driver_status = 0;
    s->queue_sel = 0;
    s->test_sent = false;
    memset(s->queues, 0, sizeof(s->queues));
    s->queues[VUSB_TX].done = true;
    while ((msg = g_queue_pop_head(&s->host_msgs))) {
        g_byte_array_unref(msg);
    }
}

static void vusb_realize(DeviceState *dev, Error **errp)
{
    VUSBState *s = VUSB(dev);

    qemu_chr_fe_set_handlers(&s->chr, vusb_chr_can_receive, vusb_chr_receive,
                             NULL, NULL, s, NULL, true);
}

static void vusb_init(Object *obj)
{
    VUSBState *s = VUSB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &vusb_ops, s, "vresearch-vusb",
                          VUSB_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    g_queue_init(&s->host_msgs);
    s->rx_buf = g_byte_array_new();
}

static const Property vusb_properties[] = {
    DEFINE_PROP_CHR("chardev", VUSBState, chr),
    DEFINE_PROP_BOOL("test-enum", VUSBState, test_enum, false),
};

static void vusb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, vusb_reset);
    device_class_set_props(dc, vusb_properties);
    dc->realize = vusb_realize;
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
