/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Esperas sobre un bit o una variable: colas compartidas indexadas por la
 * direccion (tabla de la capa). wake_up_bit()/wake_up_var() despiertan a
 * todos los de esa cola; cada uno vuelve a comprobar su condicion.
 */
#ifndef _LKPI_LINUX_WAIT_BIT_H
#define _LKPI_LINUX_WAIT_BIT_H

#include <linux/wait.h>
#include <linux/bitops.h>

struct wait_queue_head *lkpi_bit_waitqueue(const void *addr);
#define bit_waitqueue(word, bit) lkpi_bit_waitqueue(word)
#define __var_waitqueue(var) lkpi_bit_waitqueue(var)

#define wake_up_var(var) wake_up_all(lkpi_bit_waitqueue(var))
#define wake_up_bit(word, bit) wake_up_all(lkpi_bit_waitqueue(word))

#define wait_var_event(var, cond) wait_event(*lkpi_bit_waitqueue(var), cond)
#define wait_var_event_killable(var, cond) wait_event_killable(*lkpi_bit_waitqueue(var), cond)
#define wait_var_event_interruptible(var, cond) \
	wait_event_interruptible(*lkpi_bit_waitqueue(var), cond)
#define wait_var_event_timeout(var, cond, tmo) \
	wait_event_timeout(*lkpi_bit_waitqueue(var), cond, tmo)
#define wait_var_event_any_lock(var, cond, lock, type, state) \
	wait_event(*lkpi_bit_waitqueue(var), cond)

static inline int wait_on_bit(unsigned long *word, int bit, unsigned int mode)
{
	(void)mode;
	wait_event(*lkpi_bit_waitqueue(word), !test_bit(bit, word));
	return 0;
}
#define wait_on_bit_io(word, bit, mode) wait_on_bit(word, bit, mode)
static inline int wait_on_bit_timeout(unsigned long *word, int bit, unsigned int mode,
				      unsigned long timeout)
{
	(void)mode;
	return wait_event_timeout(*lkpi_bit_waitqueue(word), !test_bit(bit, word), timeout) ?
		       0 : -EAGAIN;
}
static inline int wait_on_bit_lock(unsigned long *word, int bit, unsigned int mode)
{
	(void)mode;
	wait_event(*lkpi_bit_waitqueue(word), !test_and_set_bit(bit, word));
	return 0;
}
static inline void clear_and_wake_up_bit(int bit, unsigned long *word)
{
	clear_bit_unlock(bit, word);
	smp_mb__after_atomic();
	wake_up_bit(word, bit);
}


#define ___wait_var_event(var, cond, state, exclusive, ret, cmd) \
	__lkpi_wait_event(*lkpi_bit_waitqueue(var), cond, state, exclusive, ret, cmd)
#endif
