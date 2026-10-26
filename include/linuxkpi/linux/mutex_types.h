/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_MUTEX_TYPES_H
#define _LKPI_LINUX_MUTEX_TYPES_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/spinlock_types.h>

/*
 * owner: task_struct del propietario (0 = libre) con el bit 0 a 1 si hay
 * esperas en wait_list. Al soltar con esperas, el cerrojo pasa directamente
 * al primero de la lista (orden FIFO, sin inanicion).
 */
struct mutex {
	atomic_long_t owner;
	raw_spinlock_t wait_lock;
	struct list_head wait_list;
};

#endif
