/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Cabeceras que en Linux llegan por inclusion transitiva (a traves de
 * linux/fs.h, perf_event.h, drm_print.h...) y de las que i915 depende sin
 * incluirlas. Se fuerza con -include al compilar i915/xe (lkpi_i915_cflags.mk).
 */
#ifndef _LKPI_I915_PRELUDE_H
#define _LKPI_I915_PRELUDE_H
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/sysfs.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/rculist.h>
#include <linux/kstrtox.h>
#include <linux/uaccess.h>
#include <linux/wait_bit.h>
#include <linux/irq_work.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/typecheck.h>
#include <linux/jump_label.h>
#include <linux/highmem.h>
#include <linux/uuid.h>
#include <linux/ratelimit.h>
#include <linux/iosys-map.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/kstrtox.h>
#endif
