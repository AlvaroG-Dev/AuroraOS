// kernel/usb/usb_storage.h
#ifndef KERNEL_USB_STORAGE_H
#define KERNEL_USB_STORAGE_H

#include "usb_core.h"

// Detecta interfaces Mass Storage (class 0x08, subclass 0x06, protocol
// 0x50 = BOT) en `dev`. Configura los bulk endpoints, ejecuta
// TEST_UNIT_READY + INQUIRY + READ_CAPACITY y registra un block_device_t
// con nombre "sdX" (el siguiente libre).
//
// Devuelve:
//   >0  número de interfaces de storage configuradas (0 o 1)
//    0  el device no es Mass Storage
//   <0  error
int usb_storage_probe(usb_device_t *dev);

#endif