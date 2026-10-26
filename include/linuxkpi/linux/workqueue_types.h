/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_WORKQUEUE_TYPES_H
#define _LKPI_LINUX_WORKQUEUE_TYPES_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/list.h>

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
struct workqueue_struct;

/*
 * data: bit 0 = PENDING (encolado o con su timer armado). El resto de campos
 * son de la capa: lista donde espera, cola en la que se encolo, numero de
 * secuencia (para flush_workqueue) y marcas internas.
 */
struct work_struct {
	atomic_long_t data;
	struct list_head entry;
	work_func_t func;
	struct workqueue_struct *__lkpi_wq;
	unsigned long __lkpi_seq;
	unsigned int __lkpi_flags;
};

#define WORK_STRUCT_PENDING_BIT 0
#define WORK_STRUCT_PENDING (1UL << WORK_STRUCT_PENDING_BIT)

#define __WORK_INITIALIZER(n, f) \
	{ .data = ATOMIC_LONG_INIT(0), .entry = { &(n).entry, &(n).entry }, .func = (f) }
#define DECLARE_WORK(n, f) struct work_struct n = __WORK_INITIALIZER(n, f)

#endif
