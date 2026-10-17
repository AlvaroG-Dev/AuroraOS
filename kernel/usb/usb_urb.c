// kernel/usb/usb_urb.c
#include "usb_urb.h"
#include "string.h"

void usb_urb_init(usb_urb_t *urb, usb_device_t *dev, usb_endpoint_t *ep,
                  usb_urb_type_t type, void *data, uint32_t length,
                  usb_urb_cb_t complete, void *context) {
  if (!urb)
    return;
  memset(urb, 0, sizeof(*urb));
  urb->dev = dev;
  urb->ep = ep;
  urb->type = type;
  urb->data = data;
  urb->length = length;
  urb->complete = complete;
  urb->context = context;
}

void usb_urb_complete(usb_urb_t *urb, int status, uint32_t actual_length) {
  if (!urb)
    return;
  urb->status = status;
  urb->actual_length = actual_length;
  if (urb->complete)
    urb->complete(urb);
}