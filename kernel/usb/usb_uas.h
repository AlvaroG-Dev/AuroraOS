#ifndef KERNEL_USB_UAS_H
#define KERNEL_USB_UAS_H

#include "usb_core.h"

int usb_uas_probe(usb_device_t *dev, usb_interface_t *itf,
                  usb_endpoint_t *cmd_ep,       // EP1 OUT, Command IU
                  usb_endpoint_t *status_ep,    // EP2 IN, Status IU
                  usb_endpoint_t *data_in_ep,   // EP3 IN, Data-In
                  usb_endpoint_t *data_out_ep); // EP4 OUT, Data-Out

#endif