// kernel/usb/usb_hid.h
#ifndef KERNEL_USB_HID_H
#define KERNEL_USB_HID_H

#include "usb_core.h"

#define USB_HID_CLASS 0x03
#define USB_HID_SUBCLASS_BOOT 0x01
#define USB_HID_PROTO_KBD 1
#define USB_HID_PROTO_MOUSE 2

#define USB_HID_REQ_GET_REPORT 0x01
#define USB_HID_REQ_GET_IDLE 0x02
#define USB_HID_REQ_GET_PROTOCOL 0x03
#define USB_HID_REQ_SET_REPORT 0x09
#define USB_HID_REQ_SET_IDLE 0x0A
#define USB_HID_REQ_SET_PROTOCOL 0x0B

// Prototipo para obtener el descriptor de report
int usb_hid_get_report_descriptor(usb_device_t *dev, uint8_t intf_num,
                                  void *buf, uint16_t len);

int usb_hid_probe(usb_device_t *dev);

#endif