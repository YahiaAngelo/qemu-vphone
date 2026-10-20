/*
 * QEMU USB HID devices
 *
 * Copyright (c) 2005 Fabrice Bellard
 * Copyright (c) 2007 OpenMoko, Inc.  (andrew@openedhand.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "hw/usb.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/input/hid.h"
#include "hw/usb/hid.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"

struct USBHIDState {
    USBDevice dev;
    USBEndpoint *intr;
    HIDState hid;
    uint32_t usb_version;
    char *display;
    uint32_t head;
    bool touch;     /* usb-touchscreen: a HID Touch Screen over the tablet's events */
    /* usb-touchscreen Home shortcuts (touch_home_check): the last buttons and the press's start. */
    uint8_t touch_buttons;
    uint16_t touch_start_y;
    bool vmapple_tablet;
    uint8_t vmapple_multiplier;
    uint8_t vmapple_smooth;
};

#define TYPE_USB_HID "usb-hid"
OBJECT_DECLARE_SIMPLE_TYPE(USBHIDState, USB_HID)

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT_MOUSE,
    STR_PRODUCT_TABLET,
    STR_PRODUCT_TOUCH,
    STR_PRODUCT_KEYBOARD,
    STR_SERIAL_COMPAT,
    STR_CONFIG_MOUSE,
    STR_CONFIG_TABLET,
    STR_CONFIG_KEYBOARD,
    STR_SERIAL_MOUSE,
    STR_SERIAL_TABLET,
    STR_SERIAL_KEYBOARD,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER]     = "QEMU",
    [STR_PRODUCT_MOUSE]    = "QEMU USB Mouse",
    [STR_PRODUCT_TABLET]   = "QEMU USB Tablet",
    [STR_PRODUCT_TOUCH]    = "Virtual USB Touch Screen",
    [STR_PRODUCT_KEYBOARD] = "QEMU USB Keyboard",
    [STR_SERIAL_COMPAT]    = "42",
    [STR_CONFIG_MOUSE]     = "HID Mouse",
    [STR_CONFIG_TABLET]    = "HID Tablet",
    [STR_CONFIG_KEYBOARD]  = "HID Keyboard",
    [STR_SERIAL_MOUSE]     = "89126",
    [STR_SERIAL_TABLET]    = "28754",
    [STR_SERIAL_KEYBOARD]  = "68284",
};

static const USBDescIface desc_iface_mouse = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x02,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                52, 0,         /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 4,
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescIface desc_iface_mouse2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x02,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                52, 0,         /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 4,
            .bInterval             = 7, /* 2 ^ (8-1) * 125 usecs = 8 ms */
        },
    },
};

static const USBDescIface desc_iface_tablet = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                74, 0,         /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescIface desc_iface_tablet2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                74, 0,         /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 4, /* 2 ^ (4-1) * 125 usecs = 1 ms */
        },
    },
};

static const USBDescIface desc_iface_keyboard = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x01, /* keyboard */
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x11, 0x01,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                0x3f, 0,       /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescIface desc_iface_keyboard2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x01, /* keyboard */
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x11, 0x01,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                0x3f, 0,       /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 7, /* 2 ^ (8-1) * 125 usecs = 8 ms */
        },
    },
};

static const USBDescDevice desc_device_mouse = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_MOUSE,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_mouse,
        },
    },
};

static const USBDescDevice desc_device_mouse2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_MOUSE,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_mouse2,
        },
    },
};

static const USBDescDevice desc_device_tablet = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_TABLET,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_tablet,
        },
    },
};

static const USBDescDevice desc_device_tablet2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_TABLET,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_tablet2,
        },
    },
};

static const USBDescDevice desc_device_keyboard = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_KEYBOARD,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_keyboard,
        },
    },
};

static const USBDescDevice desc_device_keyboard2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_KEYBOARD,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_keyboard2,
        },
    },
};

static const USBDescMSOS desc_msos_suspend = {
    .SelectiveSuspendEnabled = true,
};

static const USBDesc desc_mouse = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_MOUSE,
        .iSerialNumber     = STR_SERIAL_MOUSE,
    },
    .full = &desc_device_mouse,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_mouse2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_MOUSE,
        .iSerialNumber     = STR_SERIAL_MOUSE,
    },
    .full = &desc_device_mouse,
    .high = &desc_device_mouse2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_tablet = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TABLET,
        .iSerialNumber     = STR_SERIAL_TABLET,
    },
    .full = &desc_device_tablet,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_tablet2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TABLET,
        .iSerialNumber     = STR_SERIAL_TABLET,
    },
    .full = &desc_device_tablet,
    .high = &desc_device_tablet2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

/*
 * usb-touchscreen (vphone-linux): Virtualization.framework's USB touchscreen report descriptor
 * (com.apple.Virtualization.VirtualMachine, macOS 27): a Touch Screen of four Finger
 * collections, each a 6-bit transducer index, In Range and Touch, and X and Y over 0..0x7fff.
 * The tablet's events drive finger 0 (button 1 down is the finger touching); fingers 1-3
 * stay out of range. iOS's gestures (the home swipe among them) follow a finger by its
 * transducer index, which the earlier single-finger Tip Switch report did not carry.
 */
#define TOUCH_FINGER \
    0x05, 0x0d,         /*   Usage Page (Digitizer) */ \
    0x09, 0x22,         /*   Usage (Finger) */ \
    0xa1, 0x02,         /*   Collection (Logical) */ \
    0x09, 0x38,         /*     Usage (Transducer Index) */ \
    0x75, 0x06,         /*     Report Size (6) */ \
    0x95, 0x01,         /*     Report Count (1) */ \
    0x81, 0x02,         /*     Input (Data, Variable, Absolute) */ \
    0x09, 0x32,         /*     Usage (In Range) */ \
    0x09, 0x33,         /*     Usage (Touch) */ \
    0x15, 0x00,         /*     Logical Minimum (0) */ \
    0x25, 0x01,         /*     Logical Maximum (1) */ \
    0x75, 0x01,         /*     Report Size (1) */ \
    0x95, 0x02,         /*     Report Count (2) */ \
    0x81, 0x02,         /*     Input (Data, Variable, Absolute) */ \
    0x05, 0x01,         /*     Usage Page (Generic Desktop) */ \
    0x09, 0x30,         /*     Usage (X) */ \
    0x09, 0x31,         /*     Usage (Y) */ \
    0x15, 0x00,         /*     Logical Minimum (0) */ \
    0x26, 0xff, 0x7f,   /*     Logical Maximum (0x7fff) */ \
    0x75, 0x10,         /*     Report Size (16) */ \
    0x95, 0x02,         /*     Report Count (2) */ \
    0x81, 0x02,         /*     Input (Data, Variable, Absolute) */ \
    0xc0                /*   End Collection */

#define TOUCH_FINGERS 4
#define TOUCH_REPORT_SIZE (TOUCH_FINGERS * 5)

static const uint8_t qemu_touch_hid_report_descriptor[] = {
    0x05, 0x0d,         /* Usage Page (Digitizer) */
    0x09, 0x04,         /* Usage (Touch Screen) */
    0xa1, 0x01,         /* Collection (Application) */
    TOUCH_FINGER,
    TOUCH_FINGER,
    TOUCH_FINGER,
    TOUCH_FINGER,
    0xc0,               /* End Collection */
};

static const USBDescIface desc_iface_touch = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            .data = (uint8_t[]) {
                0x09, USB_DT_HID, 0x11, 0x01, 0x00, 0x01, USB_DT_REPORT,
                sizeof(qemu_touch_hid_report_descriptor), 0,
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = TOUCH_REPORT_SIZE,
            .bInterval             = 4,
        },
    },
};

static const USBDescDevice desc_device_touch = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_TABLET,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_touch,
        },
    },
};

/* VZ's touchscreen's identity: Apple, 0x8108, "Virtual USB Touch Screen". */
static const USBDesc desc_touch = {
    .id = {
        .idVendor          = 0x05ac,
        .idProduct         = 0x8108,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TOUCH,
        .iSerialNumber     = STR_SERIAL_TABLET,
    },
    .full = &desc_device_touch,
    .high = &desc_device_touch,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

/*
 * The absolute USB pointer exposed by Virtualization.framework. The iOS
 * AppleVirtualPlatformHIDEventDriver personality matches this HID identity.
 * This USB device does not itself set the IOHID "Built-In" property, so it is
 * an experiment rather than a proven replacement for the VirtIO HID bridge.
 * Keep it separate from usb-touchscreen: it is a mouse-class pointer, not a
 * USB digitizer.
 */
static const uint8_t vmapple_tablet_hid_report_descriptor[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xa1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*   Report ID (1) */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xa1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (1) */
    0x29, 0x03,             /*     Usage Maximum (3) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x95, 0x03,             /*     Report Count (3) */
    0x75, 0x01,             /*     Report Size (1) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
    0x95, 0x01,             /*     Report Count (1) */
    0x75, 0x05,             /*     Report Size (5) */
    0x81, 0x01,             /*     Input (Constant) */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x26, 0xff, 0x7f,       /*     Logical Maximum (0x7fff) */
    0x75, 0x10,             /*     Report Size (16) */
    0x95, 0x02,             /*     Report Count (2) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
    0xa1, 0x02,             /*     Collection (Logical) */
    0x85, 0x02,             /*       Report ID (2) */
    0x09, 0x48,             /*       Usage (Resolution Multiplier) */
    0x95, 0x01,             /*       Report Count (1) */
    0x75, 0x08,             /*       Report Size (8) */
    0x15, 0x00,             /*       Logical Minimum (0) */
    0x25, 0x01,             /*       Logical Maximum (1) */
    0x35, 0x01,             /*       Physical Minimum (1) */
    0x45, 0x78,             /*       Physical Maximum (120) */
    0xb1, 0x02,             /*       Feature (Data, Variable, Absolute) */
    0x85, 0x01,             /*       Report ID (1) */
    0x09, 0x38,             /*       Usage (Wheel) */
    0x35, 0x00,             /*       Physical Minimum (0) */
    0x45, 0x00,             /*       Physical Maximum (0) */
    0x16, 0x01, 0x80,       /*       Logical Minimum (-0x7fff) */
    0x26, 0xff, 0x7f,       /*       Logical Maximum (0x7fff) */
    0x75, 0x10,             /*       Report Size (16) */
    0x95, 0x01,             /*       Report Count (1) */
    0x81, 0x06,             /*       Input (Data, Variable, Relative) */
    0x05, 0x0c,             /*       Usage Page (Consumer) */
    0x0a, 0x38, 0x02,       /*       Usage (AC Pan) */
    0x81, 0x06,             /*       Input (Data, Variable, Relative) */
    0x06, 0x61, 0xff,       /*       Usage Page (0xff61, vendor-defined) */
    0x09, 0x02,             /*       Usage (2): vertical smooth scroll */
    0x09, 0x01,             /*       Usage (1): horizontal smooth scroll */
    0x75, 0x20,             /*       Report Size (32) */
    0x95, 0x02,             /*       Report Count (2) */
    0x81, 0x06,             /*       Input (Data, Variable, Relative) */
    0x09, 0x03,             /*       Usage (3): scroll phases */
    0x75, 0x10,             /*       Report Size (16) */
    0x95, 0x01,             /*       Report Count (1) */
    0x81, 0x02,             /*       Input (Data, Variable, Absolute) */
    0x85, 0x03,             /*       Report ID (3) */
    0x09, 0x04,             /*       Usage (4): smooth scrolling enabled */
    0x95, 0x01,             /*       Report Count (1) */
    0x75, 0x01,             /*       Report Size (1) */
    0xb1, 0x02,             /*       Feature (Data, Variable, Absolute) */
    0x95, 0x01,             /*       Report Count (1) */
    0x75, 0x07,             /*       Report Size (7) */
    0xb1, 0x01,             /*       Feature (Constant) */
    0xc0,                   /*     End Collection */
    0xc0,                   /*   End Collection */
    0xc0,                   /* End Collection */
};

static const USBDescIface desc_iface_vmapple_tablet = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            .data = (uint8_t[]) {
                0x09, USB_DT_HID, 0x11, 0x01, 0x00, 0x01, USB_DT_REPORT,
                sizeof(vmapple_tablet_hid_report_descriptor), 0,
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 20,
            .bInterval             = 4,
        },
    },
};

static const USBDescDevice desc_device_vmapple_tablet = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_TABLET,
            .bmAttributes          = USB_CFG_ATT_ONE,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_vmapple_tablet,
        },
    },
};

static const USBDesc desc_vmapple_tablet = {
    .id = {
        .idVendor          = 0x05ac,
        .idProduct         = 0x8106,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TABLET,
        .iSerialNumber     = STR_SERIAL_TABLET,
    },
    .full = &desc_device_vmapple_tablet,
    .high = &desc_device_vmapple_tablet,
    .str  = desc_strings,
};

#define VMAPPLE_TABLET_REPORT_INPUT       1
#define VMAPPLE_TABLET_REPORT_MULTIPLIER  2
#define VMAPPLE_TABLET_REPORT_SMOOTH      3
#define VMAPPLE_TABLET_REPORT_SIZE        20

static const USBDesc desc_keyboard = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_KEYBOARD,
        .iSerialNumber     = STR_SERIAL_KEYBOARD,
    },
    .full = &desc_device_keyboard,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_keyboard2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_KEYBOARD,
        .iSerialNumber     = STR_SERIAL_KEYBOARD,
    },
    .full = &desc_device_keyboard,
    .high = &desc_device_keyboard2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const uint8_t qemu_mouse_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x02,		/* Usage (Mouse) */
    0xa1, 0x01,		/* Collection (Application) */
    0x09, 0x01,		/*   Usage (Pointer) */
    0xa1, 0x00,		/*   Collection (Physical) */
    0x05, 0x09,		/*     Usage Page (Button) */
    0x19, 0x01,		/*     Usage Minimum (1) */
    0x29, 0x05,		/*     Usage Maximum (5) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x25, 0x01,		/*     Logical Maximum (1) */
    0x95, 0x05,		/*     Report Count (5) */
    0x75, 0x01,		/*     Report Size (1) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*     Report Count (1) */
    0x75, 0x03,		/*     Report Size (3) */
    0x81, 0x01,		/*     Input (Constant) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x30,		/*     Usage (X) */
    0x09, 0x31,		/*     Usage (Y) */
    0x09, 0x38,		/*     Usage (Wheel) */
    0x15, 0x81,		/*     Logical Minimum (-0x7f) */
    0x25, 0x7f,		/*     Logical Maximum (0x7f) */
    0x75, 0x08,		/*     Report Size (8) */
    0x95, 0x03,		/*     Report Count (3) */
    0x81, 0x06,		/*     Input (Data, Variable, Relative) */
    0xc0,		/*   End Collection */
    0xc0,		/* End Collection */
};

static const uint8_t qemu_tablet_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x02,		/* Usage (Mouse) */
    0xa1, 0x01,		/* Collection (Application) */
    0x09, 0x01,		/*   Usage (Pointer) */
    0xa1, 0x00,		/*   Collection (Physical) */
    0x05, 0x09,		/*     Usage Page (Button) */
    0x19, 0x01,		/*     Usage Minimum (1) */
    0x29, 0x05,		/*     Usage Maximum (5) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x25, 0x01,		/*     Logical Maximum (1) */
    0x95, 0x05,		/*     Report Count (5) */
    0x75, 0x01,		/*     Report Size (1) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*     Report Count (1) */
    0x75, 0x03,		/*     Report Size (3) */
    0x81, 0x01,		/*     Input (Constant) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x30,		/*     Usage (X) */
    0x09, 0x31,		/*     Usage (Y) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x26, 0xff, 0x7f,	/*     Logical Maximum (0x7fff) */
    0x35, 0x00,		/*     Physical Minimum (0) */
    0x46, 0xff, 0x7f,	/*     Physical Maximum (0x7fff) */
    0x75, 0x10,		/*     Report Size (16) */
    0x95, 0x02,		/*     Report Count (2) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x38,		/*     Usage (Wheel) */
    0x15, 0x81,		/*     Logical Minimum (-0x7f) */
    0x25, 0x7f,		/*     Logical Maximum (0x7f) */
    0x35, 0x00,		/*     Physical Minimum (same as logical) */
    0x45, 0x00,		/*     Physical Maximum (same as logical) */
    0x75, 0x08,		/*     Report Size (8) */
    0x95, 0x01,		/*     Report Count (1) */
    0x81, 0x06,		/*     Input (Data, Variable, Relative) */
    0xc0,		/*   End Collection */
    0xc0,		/* End Collection */
};

static const uint8_t qemu_keyboard_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x06,		/* Usage (Keyboard) */
    0xa1, 0x01,		/* Collection (Application) */
    0x75, 0x01,		/*   Report Size (1) */
    0x95, 0x08,		/*   Report Count (8) */
    0x05, 0x07,		/*   Usage Page (Key Codes) */
    0x19, 0xe0,		/*   Usage Minimum (224) */
    0x29, 0xe7,		/*   Usage Maximum (231) */
    0x15, 0x00,		/*   Logical Minimum (0) */
    0x25, 0x01,		/*   Logical Maximum (1) */
    0x81, 0x02,		/*   Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*   Report Count (1) */
    0x75, 0x08,		/*   Report Size (8) */
    0x81, 0x01,		/*   Input (Constant) */
    0x95, 0x05,		/*   Report Count (5) */
    0x75, 0x01,		/*   Report Size (1) */
    0x05, 0x08,		/*   Usage Page (LEDs) */
    0x19, 0x01,		/*   Usage Minimum (1) */
    0x29, 0x05,		/*   Usage Maximum (5) */
    0x91, 0x02,		/*   Output (Data, Variable, Absolute) */
    0x95, 0x01,		/*   Report Count (1) */
    0x75, 0x03,		/*   Report Size (3) */
    0x91, 0x01,		/*   Output (Constant) */
    0x95, 0x06,		/*   Report Count (6) */
    0x75, 0x08,		/*   Report Size (8) */
    0x15, 0x00,		/*   Logical Minimum (0) */
    0x25, 0xff,		/*   Logical Maximum (255) */
    0x05, 0x07,		/*   Usage Page (Key Codes) */
    0x19, 0x00,		/*   Usage Minimum (0) */
    0x29, 0xff,		/*   Usage Maximum (255) */
    0x81, 0x00,		/*   Input (Data, Array) */
    0xc0,		/* End Collection */
};

static void usb_hid_changed(HIDState *hs)
{
    USBHIDState *us = container_of(hs, USBHIDState, hid);

    usb_wakeup(us->intr, 0);
}

static void usb_hid_handle_reset(USBDevice *dev)
{
    USBHIDState *us = USB_HID(dev);

    hid_reset(&us->hid);
    if (us->vmapple_tablet) {
        us->vmapple_multiplier = 0;
        us->vmapple_smooth = 0;
    }
}

static int usb_hid_pointer_poll(USBHIDState *us, uint8_t *buf, int len);
static int usb_vmapple_tablet_poll(USBHIDState *us, uint8_t *buf, int len);

static void usb_hid_handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    USBHIDState *us = USB_HID(dev);
    HIDState *hs = &us->hid;
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
        /* hid specific requests */
    case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        switch (value >> 8) {
        case 0x22:
            if (hs->kind == HID_MOUSE) {
                memcpy(data, qemu_mouse_hid_report_descriptor,
                       sizeof(qemu_mouse_hid_report_descriptor));
                p->actual_length = sizeof(qemu_mouse_hid_report_descriptor);
            } else if (us->vmapple_tablet) {
                memcpy(data, vmapple_tablet_hid_report_descriptor,
                       sizeof(vmapple_tablet_hid_report_descriptor));
                p->actual_length = sizeof(vmapple_tablet_hid_report_descriptor);
            } else if (us->touch) {
                memcpy(data, qemu_touch_hid_report_descriptor,
                       sizeof(qemu_touch_hid_report_descriptor));
                p->actual_length = sizeof(qemu_touch_hid_report_descriptor);
            } else if (hs->kind == HID_TABLET) {
                memcpy(data, qemu_tablet_hid_report_descriptor,
                       sizeof(qemu_tablet_hid_report_descriptor));
                p->actual_length = sizeof(qemu_tablet_hid_report_descriptor);
            } else if (hs->kind == HID_KEYBOARD) {
                memcpy(data, qemu_keyboard_hid_report_descriptor,
                       sizeof(qemu_keyboard_hid_report_descriptor));
                p->actual_length = sizeof(qemu_keyboard_hid_report_descriptor);
            }
            break;
        default:
            goto fail;
        }
        break;
    case HID_GET_REPORT:
        if (us->vmapple_tablet) {
            uint8_t report_type = value >> 8;
            uint8_t report_id = value & 0xff;

            if (report_type == 1 &&
                report_id == VMAPPLE_TABLET_REPORT_INPUT) {
                p->actual_length = usb_vmapple_tablet_poll(us, data, length);
            } else if (report_type == 3 &&
                       (report_id == VMAPPLE_TABLET_REPORT_MULTIPLIER ||
                        report_id == VMAPPLE_TABLET_REPORT_SMOOTH) &&
                       length >= 2) {
                /* Numbered control reports include the report ID in-band. */
                data[0] = report_id;
                data[1] = report_id == VMAPPLE_TABLET_REPORT_MULTIPLIER
                    ? us->vmapple_multiplier : us->vmapple_smooth;
                p->actual_length = 2;
            } else {
                goto fail;
            }
        } else if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
            p->actual_length = usb_hid_pointer_poll(us, data, length);
        } else if (hs->kind == HID_KEYBOARD) {
            p->actual_length = hid_keyboard_poll(hs, data, length);
        }
        break;
    case HID_SET_REPORT:
        if (us->vmapple_tablet && (value >> 8) == 3 &&
            length >= 2 && data[0] == (value & 0xff) &&
            (value & 0xff) >= VMAPPLE_TABLET_REPORT_MULTIPLIER &&
            (value & 0xff) <= VMAPPLE_TABLET_REPORT_SMOOTH) {
            if ((value & 0xff) == VMAPPLE_TABLET_REPORT_MULTIPLIER) {
                us->vmapple_multiplier = data[1] & 1;
            } else {
                us->vmapple_smooth = data[1] & 1;
            }
            p->actual_length = length;
        } else if (hs->kind == HID_KEYBOARD) {
            p->actual_length = hid_keyboard_write(hs, data, length);
        } else {
            goto fail;
        }
        break;
    case HID_GET_PROTOCOL:
        if (hs->kind != HID_KEYBOARD && hs->kind != HID_MOUSE) {
            goto fail;
        }
        data[0] = hs->protocol;
        p->actual_length = 1;
        break;
    case HID_SET_PROTOCOL:
        if (hs->kind != HID_KEYBOARD && hs->kind != HID_MOUSE) {
            goto fail;
        }
        hs->protocol = value;
        break;
    case HID_GET_IDLE:
        data[0] = hs->idle;
        p->actual_length = 1;
        break;
    case HID_SET_IDLE:
        /*
         * vphone-linux: remember the idle rate (GET_IDLE answers it) but do
         * not repeat unchanged reports. iOS sets 24 ms on the keyboard, so
         * an idle VM sent ~42 "no keys" reports a second for backboardd and
         * the HID driver to process (about a fifth of a vCPU at rest).
         * HID reports are state: a repeat says nothing new.
         */
        hs->idle = (uint8_t) (value >> 8);
        if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
            hid_pointer_activate(hs);
        }
        break;
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

/*
 * usb-touchscreen Home shortcuts (vphone-linux). iOS's home swipe does not start from this
 * touchscreen (Control Center's top-edge swipe does), so the device sends the guest keyboard's
 * Cmd+H, iOS's Home shortcut, for a drag that starts on the bottom edge and ends at least 12% of
 * the screen higher, and for a middle click. VPHONE_TOUCH_HOME=0 turns both off.
 */
#define TOUCH_EDGE_Y (0x7fff - 0x7fff / 100)   /* the bottom 1% */
#define TOUCH_HOME_RISE (0x7fff * 12 / 100)

static void touch_send_home_bh(void *opaque)
{
    qemu_input_event_send_key_qcode(NULL, Q_KEY_CODE_META_L, true);
    qemu_input_event_send_key_qcode(NULL, Q_KEY_CODE_H, true);
    qemu_input_event_send_key_qcode(NULL, Q_KEY_CODE_H, false);
    qemu_input_event_send_key_qcode(NULL, Q_KEY_CODE_META_L, false);
}

/*
 * After the touchscreen's packet completes: the keyboard's events wake its endpoint, which must
 * not run inside this device's packet handling.
 */
static void touch_send_home(void)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), touch_send_home_bh, NULL);
}

static void touch_home_check(USBHIDState *us, uint8_t buttons, uint16_t y)
{
    static int enabled = -1;
    uint8_t pressed = buttons & ~us->touch_buttons;
    uint8_t released = us->touch_buttons & ~buttons;

    if (enabled < 0) {
        const char *e = getenv("VPHONE_TOUCH_HOME");
        enabled = !(e && e[0] == '0');
    }
    us->touch_buttons = buttons;
    if (!enabled) {
        return;
    }
    if (pressed & 1) {
        us->touch_start_y = y;
    }
    if ((released & 1) && us->touch_start_y >= TOUCH_EDGE_Y &&
        y + TOUCH_HOME_RISE <= us->touch_start_y) {
        touch_send_home();
    }
    if (pressed & 4) {
        touch_send_home();
    }
}

/* A pointer report: the tablet's, or a usb-touchscreen's made of it. */
static int usb_hid_pointer_poll(USBHIDState *us, uint8_t *buf, int len)
{
    uint8_t tablet[8];
    int n;

    if (us->vmapple_tablet) {
        return usb_vmapple_tablet_poll(us, buf, len);
    }
    if (!us->touch) {
        return hid_pointer_poll(&us->hid, buf, len);
    }
    n = hid_pointer_poll(&us->hid, tablet, sizeof(tablet));
    if (n < 5 || len < TOUCH_REPORT_SIZE) {
        return 0;
    }
    /* Each finger: index (bits 0-5), In Range (6), Touch (7), then X and Y. */
    memset(buf, 0, TOUCH_REPORT_SIZE);
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        buf[i * 5] = i;
    }
    if (tablet[0] & 1) {
        buf[0] |= 0xc0;
    }
    memcpy(buf + 1, tablet + 1, 4);
    touch_home_check(us, tablet[0], tablet[3] | (tablet[4] << 8));
    {
        /* VPHONE_TOUCH_TRACE (temporary): each report as the guest takes it, CLOCK_BOOTTIME. */
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("VPHONE_TOUCH_TRACE");
            on = e && *e && *e != '0';
        }
        if (on) {
            struct timespec ts;
            clock_gettime(CLOCK_BOOTTIME, &ts);
            fprintf(stderr, "touchrep %" PRIu64 " btn %d x %d y %d\n",
                    (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec, tablet[0],
                    tablet[1] | (tablet[2] << 8), tablet[3] | (tablet[4] << 8));
        }
    }
    return TOUCH_REPORT_SIZE;
}

/*
 * Make the QEMU absolute-pointer report look like the VZ tablet's report 1.
 * The unimplemented scroll fields and phase are zero-filled.
 */
static int usb_vmapple_tablet_poll(USBHIDState *us, uint8_t *buf, int len)
{
    uint8_t tablet[8];
    uint8_t report[VMAPPLE_TABLET_REPORT_SIZE] = { 0 };
    uint16_t wheel;
    int n;

    n = hid_pointer_poll(&us->hid, tablet, sizeof(tablet));
    if (n < 5) {
        return 0;
    }

    report[0] = VMAPPLE_TABLET_REPORT_INPUT;
    report[1] = tablet[0] & 0x07;
    memcpy(report + 2, tablet + 1, 4);
    if (n >= 6) {
        wheel = (uint16_t)((int8_t)tablet[5] *
                           (us->vmapple_multiplier ? 120 : 1));
        report[6] = wheel & 0xff;
        report[7] = wheel >> 8;
    }
    len = MIN(len, VMAPPLE_TABLET_REPORT_SIZE);
    memcpy(buf, report, len);
    return len;
}

static void usb_hid_handle_data(USBDevice *dev, USBPacket *p)
{
    USBHIDState *us = USB_HID(dev);
    HIDState *hs = &us->hid;
    g_autofree uint8_t *buf = g_malloc(p->iov.size);
    int len = 0;

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == 1) {
            if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
                hid_pointer_activate(hs);
            }
            if (!hid_has_events(hs)) {
                p->status = USB_RET_NAK;
                return;
            }
            if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
                len = usb_hid_pointer_poll(us, buf, p->iov.size);
            } else if (hs->kind == HID_KEYBOARD) {
                len = hid_keyboard_poll(hs, buf, p->iov.size);
            }
            usb_packet_copy(p, buf, len);
        } else {
            goto fail;
        }
        break;
    case USB_TOKEN_OUT:
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_hid_unrealize(USBDevice *dev)
{
    USBHIDState *us = USB_HID(dev);

    hid_free(&us->hid);
}

static void usb_hid_initfn(USBDevice *dev, int kind,
                           const USBDesc *usb1, const USBDesc *usb2,
                           Error **errp)
{
    USBHIDState *us = USB_HID(dev);
    switch (us->usb_version) {
    case 1:
        dev->usb_desc = usb1;
        break;
    case 2:
        dev->usb_desc = usb2;
        break;
    default:
        dev->usb_desc = NULL;
    }
    if (!dev->usb_desc) {
        error_setg(errp, "Invalid usb version %d for usb hid device",
                   us->usb_version);
        return;
    }

    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    us->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    hid_init(&us->hid, kind, usb_hid_changed);
    if (us->display && us->hid.s) {
        qemu_input_handler_bind(us->hid.s, us->display, us->head, NULL);
    }
}

static void usb_tablet_realize(USBDevice *dev, Error **errp)
{

    usb_hid_initfn(dev, HID_TABLET, &desc_tablet, &desc_tablet2, errp);
}

static void usb_touch_realize(USBDevice *dev, Error **errp)
{
    USB_HID(dev)->touch = true;
    usb_hid_initfn(dev, HID_TABLET, &desc_touch, &desc_touch, errp);
}

static void usb_vmapple_tablet_realize(USBDevice *dev, Error **errp)
{
    USB_HID(dev)->vmapple_tablet = true;
    usb_hid_initfn(dev, HID_TABLET, &desc_vmapple_tablet,
                   &desc_vmapple_tablet, errp);
}

static void usb_mouse_realize(USBDevice *dev, Error **errp)
{
    usb_hid_initfn(dev, HID_MOUSE, &desc_mouse, &desc_mouse2, errp);
}

static void usb_keyboard_realize(USBDevice *dev, Error **errp)
{
    usb_hid_initfn(dev, HID_KEYBOARD, &desc_keyboard, &desc_keyboard2, errp);
}

static int usb_ptr_post_load(void *opaque, int version_id)
{
    USBHIDState *s = opaque;

    if (s->dev.remote_wakeup) {
        hid_pointer_activate(&s->hid);
    }
    return 0;
}

static const VMStateDescription vmstate_usb_ptr = {
    .name = "usb-ptr",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = usb_ptr_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, USBHIDState),
        VMSTATE_HID_POINTER_DEVICE(hid, USBHIDState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_usb_kbd = {
    .name = "usb-kbd",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, USBHIDState),
        VMSTATE_HID_KEYBOARD_DEVICE(hid, USBHIDState),
        VMSTATE_END_OF_LIST()
    }
};

static void usb_hid_class_initfn(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->handle_reset   = usb_hid_handle_reset;
    uc->handle_control = usb_hid_handle_control;
    uc->handle_data    = usb_hid_handle_data;
    uc->unrealize      = usb_hid_unrealize;
    uc->handle_attach  = usb_desc_attach;
}

static const TypeInfo usb_hid_type_info = {
    .name = TYPE_USB_HID,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBHIDState),
    .abstract = true,
    .class_init = usb_hid_class_initfn,
};

static const Property usb_tablet_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
        DEFINE_PROP_STRING("display", USBHIDState, display),
        DEFINE_PROP_UINT32("head", USBHIDState, head, 0),
};

static void usb_tablet_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_tablet_realize;
    uc->product_desc   = "QEMU USB Tablet";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_tablet_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_tablet_info = {
    .name          = "usb-tablet",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_tablet_class_initfn,
};

static void usb_touch_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_touch_realize;
    uc->product_desc   = "QEMU USB Touchscreen";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_tablet_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_touch_info = {
    .name          = "usb-touchscreen",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_touch_class_initfn,
};

static void usb_vmapple_tablet_class_initfn(ObjectClass *klass,
                                            const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_vmapple_tablet_realize;
    uc->product_desc   = "QEMU VZ-compatible USB Tablet";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_tablet_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_vmapple_tablet_info = {
    .name          = "usb-vmapple-tablet",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_vmapple_tablet_class_initfn,
};

static const Property usb_mouse_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
};

static void usb_mouse_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_mouse_realize;
    uc->product_desc   = "QEMU USB Mouse";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_mouse_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_mouse_info = {
    .name          = "usb-mouse",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_mouse_class_initfn,
};

static const Property usb_keyboard_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
        DEFINE_PROP_STRING("display", USBHIDState, display),
};

static void usb_keyboard_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_keyboard_realize;
    uc->product_desc   = "QEMU USB Keyboard";
    dc->vmsd = &vmstate_usb_kbd;
    device_class_set_props(dc, usb_keyboard_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_keyboard_info = {
    .name          = "usb-kbd",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_keyboard_class_initfn,
};

static void usb_hid_register_types(void)
{
    type_register_static(&usb_hid_type_info);
    type_register_static(&usb_tablet_info);
    type_register_static(&usb_touch_info);
    type_register_static(&usb_vmapple_tablet_info);
    usb_legacy_register("usb-tablet", "tablet", NULL);
    type_register_static(&usb_mouse_info);
    usb_legacy_register("usb-mouse", "mouse", NULL);
    type_register_static(&usb_keyboard_info);
    usb_legacy_register("usb-kbd", "keyboard", NULL);
}

type_init(usb_hid_register_types)
