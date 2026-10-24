/*
 * Apple VirtIO USB device-mode controller for vresearch101.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AVPBooter and iBoot use the backdoor MMIO transport implemented in vusb.c.
 * RestoreOS binds to PCI 106b:1a01 and uses the same USB message stream over
 * VirtIO queues. Queue 0 carries host-to-guest requests and queue 1 carries
 * guest-to-host responses. It reuses vusb.c's message
 * framing so the USB/IP bridge and MMIO transport share one device-side USB
 * link. The Apple queue ABI is inferred and needs validation against a live
 * RestoreOS guest.
 */

#include "qemu/osdep.h"
#include "hw/vmapple/vmapple.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/qdev-properties.h"
#include "qemu/iov.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "trace.h"

#define TYPE_VMAPPLE_VUSB_DEVICE "vresearch-vusb-device"
OBJECT_DECLARE_SIMPLE_TYPE(VUSBVirtIODevice, VMAPPLE_VUSB_DEVICE)

OBJECT_DECLARE_SIMPLE_TYPE(VUSBPCIDevice, VMAPPLE_VUSB_PCI)

#define VUSB_VIRTIO_DEVICE_ID 0x1a01
#define VUSB_QUEUE_SIZE       128
#define VUSB_QUEUE_COUNT      2
#define VUSB_MSG_MAX          0x10000
#define VUSB_QUEUE_TO_GUEST   0
#define VUSB_QUEUE_TO_HOST    1

struct VUSBVirtIODevice {
    VirtIODevice parent_obj;

    DeviceState *vusb;
    VirtQueue *to_host;
    VirtQueue *to_guest;
    bool active;
};

struct VUSBPCIDevice {
    VirtIOPCIProxy parent_obj;

    VUSBVirtIODevice vdev;
    DeviceState *vusb;
};

static size_t vusb_pci_iov_size(const struct iovec *iov, unsigned int count)
{
    size_t size = 0;
    unsigned int i;

    for (i = 0; i < count; i++) {
        if (iov[i].iov_len > VUSB_MSG_MAX - size) {
            return VUSB_MSG_MAX + 1;
        }
        size += iov[i].iov_len;
    }
    return size;
}

/* The guest is returning a USB transfer result to the host-side bridge. */
static void vusb_pci_handle_to_host(VirtIODevice *vdev, VirtQueue *vq)
{
    VUSBVirtIODevice *s = VMAPPLE_VUSB_DEVICE(vdev);
    VirtQueueElement *elem;

    if (!s->active) {
        return;
    }

    while ((elem = virtqueue_pop(vq, sizeof(*elem)))) {
        size_t len = vusb_pci_iov_size(elem->out_sg, elem->out_num);

        trace_vusb_pci_iov(VUSB_QUEUE_TO_HOST, elem->out_num, elem->in_num,
                           len, vusb_pci_iov_size(elem->in_sg, elem->in_num));
        if (!len || len > VUSB_MSG_MAX || !elem->out_num) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vusb-pci: invalid guest response descriptor\n");
        } else {
            g_autofree uint8_t *msg = g_malloc(len);
            size_t copied = iov_to_buf(elem->out_sg, elem->out_num, 0,
                                       msg, len);

            if (copied != len) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "vusb-pci: short guest response (%zu/%zu)\n",
                              copied, len);
            } else {
                trace_vusb_pci_tx(len, msg[0], len > 1 ? msg[1] : 0);
                vusb_msg_to_host(s->vusb, msg, len);
            }
        }

        virtqueue_push(vq, elem, 0);
        g_free(elem);
    }
    virtio_notify(vdev, vq);
}

/* Deliver queued host requests into the guest's writable response buffers. */
static void vusb_pci_handle_to_guest(VUSBVirtIODevice *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    VirtQueue *vq = s->to_guest;
    VirtQueueElement *elem;
    bool notified = false;

    if (!s->active) {
        return;
    }

    while ((elem = virtqueue_pop(vq, sizeof(*elem)))) {
        GByteArray *msg = vusb_pop_host_msg(s->vusb);
        size_t capacity = vusb_pci_iov_size(elem->in_sg, elem->in_num);
        size_t copied;

        trace_vusb_pci_iov(VUSB_QUEUE_TO_GUEST, elem->out_num, elem->in_num,
                           vusb_pci_iov_size(elem->out_sg, elem->out_num),
                           capacity);
        if (elem->out_num) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vusb-pci: to-guest element also has %u readable descriptor(s)\n",
                          elem->out_num);
        }
        if (!msg) {
            virtqueue_unpop(vq, elem, 0);
            g_free(elem);
            break;
        }
        if (!elem->in_num || capacity < msg->len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vusb-pci: guest buffer too small (%zu for %u-byte message)\n",
                          capacity, msg->len);
            vusb_unpop_host_msg(s->vusb, msg);
            virtqueue_unpop(vq, elem, 0);
            g_free(elem);
            break;
        }

        copied = iov_from_buf(elem->in_sg, elem->in_num, 0,
                              msg->data, msg->len);
        if (copied != msg->len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vusb-pci: short host message copy (%zu/%u)\n",
                          copied, msg->len);
            vusb_unpop_host_msg(s->vusb, msg);
            virtqueue_unpop(vq, elem, 0);
            g_free(elem);
            break;
        }

        virtqueue_push(vq, elem, copied);
        if (msg->len >= 6) {
            uint32_t head0 = 0, head1 = 0;
            for (unsigned int i = 0; i < 4 && i < msg->len; i++) {
                head0 |= (uint32_t)msg->data[i] << (8 * i);
            }
            for (unsigned int i = 4; i < 8 && i < msg->len; i++) {
                head1 |= (uint32_t)msg->data[i] << (8 * (i - 4));
            }
            trace_vusb_pci_rx(msg->len, msg->data[4], msg->data[5],
                              head0, head1);
        }
        g_byte_array_unref(msg);
        g_free(elem);
        notified = true;
    }

    if (notified) {
        virtio_notify(vdev, vq);
    }
}

static void vusb_pci_handle_guest_buffers(VirtIODevice *vdev, VirtQueue *vq)
{
    (void)vq;
    vusb_pci_handle_to_guest(VMAPPLE_VUSB_DEVICE(vdev));
}

static void vusb_pci_kick(void *opaque)
{
    vusb_pci_handle_to_guest(opaque);
}

static int vusb_pci_set_status(VirtIODevice *vdev, uint8_t status)
{
    VUSBVirtIODevice *s = VMAPPLE_VUSB_DEVICE(vdev);
    bool active = virtio_device_should_start(vdev, status);

    trace_vusb_pci_status(status, active);
    if (active != s->active) {
        s->active = active;
        vusb_alt_activate(s->vusb, active);
    }

    if (active) {
        vusb_pci_handle_to_host(vdev, s->to_host);
        vusb_pci_handle_to_guest(s);
    }
    return 0;
}

static uint64_t vusb_pci_get_features(VirtIODevice *vdev,
                                      uint64_t requested_features,
                                      Error **errp)
{
    trace_vusb_pci_features(requested_features);
    return requested_features;
}

static void vusb_pci_reset(VirtIODevice *vdev)
{
    VUSBVirtIODevice *s = VMAPPLE_VUSB_DEVICE(vdev);

    if (s->active) {
        s->active = false;
        vusb_alt_activate(s->vusb, false);
    }
}

static void vusb_device_realize(DeviceState *dev, Error **errp)
{
    VUSBVirtIODevice *s = VMAPPLE_VUSB_DEVICE(dev);
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VUSBAltTransport alt = {
        .kick = vusb_pci_kick,
        .opaque = s,
    };

    if (!s->vusb) {
        error_setg(errp, "vusb-pci requires its MMIO vusb device");
        return;
    }

    virtio_init(vdev, VUSB_VIRTIO_DEVICE_ID, 0);
    vdev->name = "vresearch-vusb";
    s->to_guest = virtio_add_queue(vdev, VUSB_QUEUE_SIZE,
                                   vusb_pci_handle_guest_buffers);
    s->to_host = virtio_add_queue(vdev, VUSB_QUEUE_SIZE,
                                  vusb_pci_handle_to_host);
    vusb_set_alt_transport(s->vusb, &alt);
}

static void vusb_device_unrealize(DeviceState *dev)
{
    VUSBVirtIODevice *s = VMAPPLE_VUSB_DEVICE(dev);
    VUSBAltTransport alt = { 0 };

    vusb_alt_activate(s->vusb, false);
    vusb_set_alt_transport(s->vusb, &alt);
    virtio_del_queue(VIRTIO_DEVICE(dev), VUSB_QUEUE_TO_HOST);
    virtio_del_queue(VIRTIO_DEVICE(dev), VUSB_QUEUE_TO_GUEST);
    virtio_cleanup(VIRTIO_DEVICE(dev));
}

static const VMStateDescription vmstate_vusb_device = {
    .name = "vresearch-vusb-device",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static void vusb_device_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_vusb_device;
    vdc->realize = vusb_device_realize;
    vdc->unrealize = vusb_device_unrealize;
    vdc->get_features = vusb_pci_get_features;
    vdc->set_status = vusb_pci_set_status;
    vdc->reset = vusb_pci_reset;
}

static const TypeInfo vusb_device_info = {
    .name          = TYPE_VMAPPLE_VUSB_DEVICE,
    .parent        = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VUSBVirtIODevice),
    .class_init    = vusb_device_class_init,
};

static const Property vusb_pci_properties[] = {
    DEFINE_PROP_UINT32("class", VirtIOPCIProxy, class_code,
                       PCI_CLASS_SERIAL_USB_DEVICE),
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_BIT("x-allow-queue-disable", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_ALLOW_QUEUE_DISABLE_BIT, true),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors,
                       DEV_NVECTORS_UNSPECIFIED),
    DEFINE_PROP_LINK("vusb", VUSBPCIDevice, vusb, TYPE_VMAPPLE_VUSB,
                     DeviceState *),
};

static void vusb_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VUSBPCIDevice *dev = VMAPPLE_VUSB_PCI(vpci_dev);
    PCIDevice *pci_dev = PCI_DEVICE(vpci_dev);

    if (!dev->vusb) {
        error_setg(errp, "vusb-pci requires a vusb link");
        return;
    }

    if (vpci_dev->nvectors == DEV_NVECTORS_UNSPECIFIED) {
        vpci_dev->nvectors = VUSB_QUEUE_COUNT + 1;
    }

    dev->vdev.vusb = dev->vusb;
    if (!qdev_realize(DEVICE(&dev->vdev), BUS(&vpci_dev->bus), errp)) {
        return;
    }

    pci_config_set_vendor_id(pci_dev->config, PCI_VENDOR_ID_APPLE);
    pci_config_set_device_id(pci_dev->config, PCI_DEVICE_ID_APPLE_VIRTIO_USB);
    /* Keep the legacy INTx line usable until the guest enables MSI-X. */
    pci_dev->config[PCI_INTERRUPT_PIN] = 1;
}

static void vusb_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, vusb_pci_properties);
    k->realize = vusb_pci_realize;
    pcidev_k->vendor_id = PCI_VENDOR_ID_APPLE;
    pcidev_k->device_id = PCI_DEVICE_ID_APPLE_VIRTIO_USB;
    pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
    /* PCIDeviceClass stores only base class + subclass (the low byte of
     * class_code carries the USB device-mode programming interface).
     */
    pcidev_k->class_id = PCI_CLASS_SERIAL_USB;
}

static void vusb_pci_instance_init(Object *obj)
{
    VUSBPCIDevice *dev = VMAPPLE_VUSB_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VMAPPLE_VUSB_DEVICE);
}

static const VirtioPCIDeviceTypeInfo vusb_pci_info = {
    .generic_name  = TYPE_VMAPPLE_VUSB_PCI,
    .instance_size = sizeof(VUSBPCIDevice),
    .instance_init = vusb_pci_instance_init,
    .class_init    = vusb_pci_class_init,
};

static void vusb_pci_register_types(void)
{
    type_register_static(&vusb_device_info);
    virtio_pci_types_register(&vusb_pci_info);
}

type_init(vusb_pci_register_types)
