/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fbdev: la capa no lo tiene (fbdev esta desactivado en L5; Aurora pinta con
 * el compositor). Solo las declaraciones adelantadas que piden las
 * cabeceras de DRM, mas los includes que Linux arrastra desde fb.h.
 */
#ifndef _LKPI_LINUX_FB_H
#define _LKPI_LINUX_FB_H

#include <linux/types.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <uapi/linux/fb.h>

struct fb_info;
struct fb_ops;
struct fb_var_screeninfo;
struct fb_fix_screeninfo;
struct fb_deferred_io;

#endif
