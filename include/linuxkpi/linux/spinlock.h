/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Spinlocks (minimo de L1, lo necesita xarray; K2 completo en L2).
 * Desactivan la expulsion del planificador de Aurora mientras se tienen;
 * las variantes _irq/_irqsave desactivan ademas las interrupciones.
 */
#ifndef _LKPI_LINUX_SPINLOCK_H
#define _LKPI_LINUX_SPINLOCK_H

#include <linux/compiler.h>
#include <linux/spinlock_types.h>
#include <linux/irqflags.h>
#include <asm/barrier.h>

void lkpi_preempt_disable(void);
void lkpi_preempt_enable(void);
#define preempt_disable() lkpi_preempt_disable()
#define preempt_enable() lkpi_preempt_enable()
#define preempt_enable_no_resched() lkpi_preempt_enable()

static __always_inline void raw_spin_lock_init(raw_spinlock_t *lock) { lock->locked = 0; }
static __always_inline bool __lkpi_spin_trylock(raw_spinlock_t *lock)
{
	return !__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE);
}
static __always_inline void __lkpi_spin_lock(raw_spinlock_t *lock)
{
	while (!__lkpi_spin_trylock(lock))
		while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
			cpu_relax();
}
static __always_inline void __lkpi_spin_unlock(raw_spinlock_t *lock)
{
	__atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

static __always_inline void raw_spin_lock(raw_spinlock_t *lock)
{
	preempt_disable();
	__lkpi_spin_lock(lock);
}
static __always_inline void raw_spin_unlock(raw_spinlock_t *lock)
{
	__lkpi_spin_unlock(lock);
	preempt_enable();
}
static __always_inline int raw_spin_trylock(raw_spinlock_t *lock)
{
	preempt_disable();
	if (__lkpi_spin_trylock(lock))
		return 1;
	preempt_enable();
	return 0;
}
static __always_inline void raw_spin_lock_irq(raw_spinlock_t *lock)
{
	local_irq_disable();
	raw_spin_lock(lock);
}
static __always_inline void raw_spin_unlock_irq(raw_spinlock_t *lock)
{
	raw_spin_unlock(lock);
	local_irq_enable();
}
#define raw_spin_lock_irqsave(lock, flags) \
	do { local_irq_save(flags); raw_spin_lock(lock); } while (0)
#define raw_spin_unlock_irqrestore(lock, flags) \
	do { raw_spin_unlock(lock); local_irq_restore(flags); } while (0)
#define raw_spin_is_locked(lock) (READ_ONCE((lock)->locked) != 0)

#define spin_lock_init(l) raw_spin_lock_init(&(l)->rlock)
#define spin_lock(l) raw_spin_lock(&(l)->rlock)
#define spin_unlock(l) raw_spin_unlock(&(l)->rlock)
#define spin_trylock(l) raw_spin_trylock(&(l)->rlock)
#define spin_lock_bh(l) spin_lock(l)
#define spin_unlock_bh(l) spin_unlock(l)
#define spin_lock_irq(l) raw_spin_lock_irq(&(l)->rlock)
#define spin_unlock_irq(l) raw_spin_unlock_irq(&(l)->rlock)
#define spin_lock_irqsave(l, flags) raw_spin_lock_irqsave(&(l)->rlock, flags)
#define spin_unlock_irqrestore(l, flags) raw_spin_unlock_irqrestore(&(l)->rlock, flags)
#define spin_lock_nested(l, sub) spin_lock(l)
#define spin_lock_irqsave_nested(l, f, sub) spin_lock_irqsave(l, f)
#define spin_is_locked(l) raw_spin_is_locked(&(l)->rlock)
#define assert_spin_locked(l) do { (void)(l); } while (0)
#define lockdep_assert_held(l) do { (void)(l); } while (0)
#define lockdep_assert_not_held(l) do { (void)(l); } while (0)
#define lockdep_assert_held_once(l) do { (void)(l); } while (0)

#endif
