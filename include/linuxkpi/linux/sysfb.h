/* SPDX-License-Identifier: GPL-2.0-only */
/* Framebuffer generico del firmware: Aurora lo gestiona el compositor (GOP). */
#ifndef _LKPI_LINUX_SYSFB_H
#define _LKPI_LINUX_SYSFB_H

#include <linux/types.h>

struct device;

static inline void sysfb_disable(struct device *dev) { }
static inline bool sysfb_handles_screen_info(void) { return false; }

#endif
