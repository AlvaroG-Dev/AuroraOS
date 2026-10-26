/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SPINLOCK_TYPES_H
#define _LKPI_LINUX_SPINLOCK_TYPES_H

#include <linux/types.h>

typedef struct raw_spinlock {
	volatile unsigned int locked;
} raw_spinlock_t;

typedef struct spinlock {
	struct raw_spinlock rlock;
} spinlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED(name) (raw_spinlock_t){ 0 }
#define __SPIN_LOCK_UNLOCKED(name) (spinlock_t){ { 0 } }
#define DEFINE_SPINLOCK(x) spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define DEFINE_RAW_SPINLOCK(x) raw_spinlock_t x = __RAW_SPIN_LOCK_UNLOCKED(x)

#endif
