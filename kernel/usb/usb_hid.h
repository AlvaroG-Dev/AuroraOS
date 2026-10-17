// kernel/usb/usb_hid.h
#ifndef KERNEL_USB_HID_H
#define KERNEL_USB_HID_H

#include "usb_core.h"

#define USB_HID_CLASS 0x03
#define USB_HID_SUBCLASS_BOOT 0x01
#define USB_HID_PROTO_KBD 1
#define USB_HID_PROTO_MOUSE 2

#define USB_HID_REQ_SET_PROTOCOL 0x0B
#define USB_HID_REQ_SET_IDLE 0x0A

// Detecta interfaces HID, configura el endpoint de interrupt IN,
// pone el device en boot protocol (SET_PROTOCOL(0)) y arranca un
// kthread que lee reports y los entrega al input subsystem + TTY.
//
// Devuelve el número de interfaces HID configuradas, o negativo.
int usb_hid_probe(usb_device_t *dev);

#endif