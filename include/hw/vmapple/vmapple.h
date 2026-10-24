/*
 * Devices specific to the VMApple machine type
 *
 * Copyright © 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_VMAPPLE_VMAPPLE_H
#define HW_VMAPPLE_VMAPPLE_H

#define TYPE_VMAPPLE_AES "vmapple-aes"

#define TYPE_VMAPPLE_BDIF "vmapple-bdif"

#define TYPE_VMAPPLE_CFG "vmapple-cfg"

#define TYPE_VMAPPLE_VIRTIO_BLK_PCI "vmapple-virtio-blk-pci"

#define TYPE_VMAPPLE_VPIOP "vresearch-vpiop"

#define TYPE_VMAPPLE_SEP_STORAGE "vresearch-sep-storage"

#define TYPE_VMAPPLE_VUSB "vresearch-vusb"
#define TYPE_VMAPPLE_VUSB_PCI "vresearch-vusb-pci"

/*
 * The USB device-mode link (vusb.c) has two guest-facing transports sharing
 * one chardev and one queue of host messages: the backdoor slot AVPBooter and
 * iBoot drive, and the VirtIO-PCI device (106b:1a01) the restore OS drives
 * (AppleVirtIOUSBDeviceController). The PCI side registers itself here.
 */
typedef struct VUSBAltTransport {
    void (*kick)(void *opaque);    /* host messages are waiting */
    void *opaque;
} VUSBAltTransport;

void vusb_set_alt_transport(DeviceState *vusb, const VUSBAltTransport *alt);
/* The alternate transport's driver started (or stopped) using the link. */
void vusb_alt_activate(DeviceState *vusb, bool active);
/* Oldest host message ({u32 len; u8 ep; u8 type; payload}), or NULL. */
GByteArray *vusb_pop_host_msg(DeviceState *vusb);
/* Give back a message popped but not delivered (no room yet). */
void vusb_unpop_host_msg(DeviceState *vusb, GByteArray *msg);
/* A device to host message ({u8 code; u8 ep; data}). */
void vusb_msg_to_host(DeviceState *vusb, const uint8_t *msg, uint32_t len);

#endif /* HW_VMAPPLE_VMAPPLE_H */
