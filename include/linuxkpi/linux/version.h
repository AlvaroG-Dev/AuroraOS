/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_VERSION_H
#define _LKPI_LINUX_VERSION_H
/* Version de la serie de Linux que emula la capa (kernel/linuxkpi/linux-version.mk). */
#define LINUX_VERSION_MAJOR 6
#define LINUX_VERSION_PATCHLEVEL 18
#define LINUX_VERSION_SUBLEVEL 59
#define LINUX_VERSION_CODE ((6 << 16) + (18 << 8) + 59)
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#endif
