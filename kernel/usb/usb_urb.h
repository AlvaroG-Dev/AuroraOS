// kernel/usb/usb_urb.h
#ifndef KERNEL_USB_URB_H
#define KERNEL_USB_URB_H

#include "usb_core.h"
#include <stdint.h>

typedef enum {
  USB_URB_CONTROL = 0,
  USB_URB_BULK = 1,
  USB_URB_INTERRUPT = 2,
  USB_URB_ISOCHRONOUS = 3,
} usb_urb_type_t;

typedef struct usb_urb usb_urb_t;

// Callback de finalización. Lo llama el HCD desde IRQ o desde el
// contexto donde complete la transferencia.
typedef void (*usb_urb_cb_t)(usb_urb_t *urb);

struct usb_urb {
  usb_device_t *dev;
  usb_endpoint_t *ep; // NULL para control en EP0
  usb_urb_type_t type;
  void *data;
  uint32_t length;
  uint32_t actual_length;
  int status; // 0 = OK, negativo = error
  usb_urb_cb_t complete;
  void *context; // para el llamante
  void *hc_priv; // HCD-privado (TRB address, etc.)
};

void usb_urb_init(usb_urb_t *urb, usb_device_t *dev, usb_endpoint_t *ep,
                  usb_urb_type_t type, void *data, uint32_t length,
                  usb_urb_cb_t complete, void *context);

// Llamado por el HCD al terminar. Setea status/actual_length y dispara
// el callback (si lo hay).
void usb_urb_complete(usb_urb_t *urb, int status, uint32_t actual_length);

#endif