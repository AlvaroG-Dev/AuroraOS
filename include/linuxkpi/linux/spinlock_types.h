/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SPINLOCK_TYPES_H
#define _LKPI_LINUX_SPINLOCK_TYPES_H

#include <linux/types.h>
#include <linux/lockdep_types.h>

/* Cerrojo de prueba y prueba-y-asigna: 0 libre, 1 cogido. */
typedef struct raw_spinlock {
	volatile unsigned int locked;
} raw_spinlock_t;

typedef struct spinlock {
	struct raw_spinlock rlock;
} spinlock_t;

/*
 * rwlock: bit 31 = escritor, bits 0..30 = lectores. Sin preferencia de
 * escritor: un lector puede volver a entrar (p. ej. desde una IRQ) aunque
 * haya un escritor esperando, como permite qrwlock en Linux.
 */
typedef struct {
	volatile unsigned int cnts;
} rwlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED(name) (raw_spinlock_t){ 0 }
#define __RAW_SPIN_LOCK_INITIALIZER(name) { 0 }
#define __SPIN_LOCK_UNLOCKED(name) (spinlock_t){ { 0 } }
#define __SPIN_LOCK_INITIALIZER(name) { { 0 } }
#define __RW_LOCK_UNLOCKED(name) (rwlock_t){ 0 }
#define DEFINE_SPINLOCK(x) spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define DEFINE_RAW_SPINLOCK(x) raw_spinlock_t x = __RAW_SPIN_LOCK_UNLOCKED(x)
#define DEFINE_RWLOCK(x) rwlock_t x = __RW_LOCK_UNLOCKED(x)

#endif
