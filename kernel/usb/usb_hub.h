// kernel/usb/usb_hub.h
#ifndef KERNEL_USB_HUB_H
#define KERNEL_USB_HUB_H

#include "usb_core.h"

// Detecta interfaces Hub (class 0x09). Para cada una: lee el Hub
// Descriptor, enciende y resetea los puertos, arranca un kthread que
// lee el interrupt endpoint, y enumera recursivamente los devices
// conectados.
//
// Devuelve el número de puertos con device conectado (>=0) o negativo.
int usb_hub_probe(usb_device_t *dev);

#endif