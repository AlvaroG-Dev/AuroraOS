// kernel/usb/usb_proxy.h
#ifndef KERNEL_USB_PROXY_H
#define KERNEL_USB_PROXY_H

#include "../vfs.h"

// VFS ops de /dev/usbproxy (devfs, ver vfs.c).
extern vfs_ops_t uproxy_dev_ops;

void uproxy_init(void);

// 1 si existe /usr/lib/aur/xhci.user: el xHCI lo gestiona xhci.aur y el
// driver de kernel xhci.c no debe reclamar el controlador (Fase 5.4).
int xhci_user_mode(void);

#endif
