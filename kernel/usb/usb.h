// kernel/usb/usb.h
#ifndef KERNEL_USB_H
#define KERNEL_USB_H

#include "usb_core.h"
#include "usb_dma.h"
#include "usb_urb.h"

// Inicializa el subsistema USB (registra drivers de clase, monta hubs
// virtuales, etc.). En Fase 0 es no-op.
void usb_init(void);

// Debug: imprime el estado del subsistema USB.
void usb_dump(void);

#endif