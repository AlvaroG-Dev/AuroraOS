/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Spinlocks y rwlocks (K2). Desactivan la expulsion del planificador de
 * Aurora mientras se tienen; las variantes _irq/_irqsave desactivan ademas
 * las interrupciones. _bh equivale a la variante simple: los bottom halves
 * de la capa corren en un hilo (lkpi_softirqd) que no puede expulsar a quien
 * tiene el cerrojo en su CPU.
 */
#ifndef _LKPI_LINUX_SPINLOCK_H
#define _LKPI_LINUX_SPINLOCK_H

#include <linux/compiler.h>
#include <linux/spinlock_types.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/bottom_half.h>
#include <linux/lockdep.h>
#include <linux/cleanup.h>
#include <linux/atomic.h>
#include <asm/barrier.h>

/* ------------------------------------------------------------ nucleo */

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

/* -------------------------------------------------------- raw_spin_* */

static __always_inline void raw_spin_lock_init(raw_spinlock_t *lock) { lock->locked = 0; }
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
static __always_inline unsigned long __lkpi_raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
	unsigned long flags;
	local_irq_save(flags);
	raw_spin_lock(lock);
	return flags;
}
static __always_inline void raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
{
	raw_spin_unlock(lock);
	local_irq_restore(flags);
}
static __always_inline int __lkpi_raw_spin_trylock_irqsave(raw_spinlock_t *lock, unsigned long *flags)
{
	local_irq_save(*flags);
	if (raw_spin_trylock(lock))
		return 1;
	local_irq_restore(*flags);
	return 0;
}
static __always_inline int raw_spin_trylock_irq(raw_spinlock_t *lock)
{
	local_irq_disable();
	if (raw_spin_trylock(lock))
		return 1;
	local_irq_enable();
	return 0;
}

#define raw_spin_lock_irqsave(lock, flags) \
	do { (flags) = __lkpi_raw_spin_lock_irqsave(lock); } while (0)
#define raw_spin_trylock_irqsave(lock, flags) __lkpi_raw_spin_trylock_irqsave(lock, &(flags))
#define raw_spin_lock_bh(lock) raw_spin_lock(lock)
#define raw_spin_unlock_bh(lock) raw_spin_unlock(lock)
#define raw_spin_trylock_bh(lock) raw_spin_trylock(lock)
#define raw_spin_lock_nested(lock, sub) raw_spin_lock(lock)
#define raw_spin_lock_irqsave_nested(lock, flags, sub) raw_spin_lock_irqsave(lock, flags)
#define raw_spin_is_locked(lock) (READ_ONCE((lock)->locked) != 0)
#define raw_spin_is_contended(lock) 0
#define assert_raw_spin_locked(lock) do { (void)(lock); } while (0)

/* ------------------------------------------------------------ spin_* */

#define spin_lock_init(l) raw_spin_lock_init(&(l)->rlock)
#define spin_lock(l) raw_spin_lock(&(l)->rlock)
#define spin_unlock(l) raw_spin_unlock(&(l)->rlock)
#define spin_trylock(l) raw_spin_trylock(&(l)->rlock)
#define spin_lock_bh(l) spin_lock(l)
#define spin_unlock_bh(l) spin_unlock(l)
#define spin_trylock_bh(l) spin_trylock(l)
#define spin_lock_irq(l) raw_spin_lock_irq(&(l)->rlock)
#define spin_unlock_irq(l) raw_spin_unlock_irq(&(l)->rlock)
#define spin_trylock_irq(l) raw_spin_trylock_irq(&(l)->rlock)
#define spin_lock_irqsave(l, flags) raw_spin_lock_irqsave(&(l)->rlock, flags)
#define spin_unlock_irqrestore(l, flags) raw_spin_unlock_irqrestore(&(l)->rlock, flags)
#define spin_trylock_irqsave(l, flags) raw_spin_trylock_irqsave(&(l)->rlock, flags)
#define spin_lock_nested(l, sub) spin_lock(l)
#define spin_lock_nest_lock(l, nl) spin_lock(l)
#define spin_lock_bh_nested(l, sub) spin_lock(l)
#define spin_lock_irqsave_nested(l, f, sub) spin_lock_irqsave(l, f)
#define spin_is_locked(l) raw_spin_is_locked(&(l)->rlock)
#define spin_is_contended(l) 0
#define spin_needbreak(l) 0
#define assert_spin_locked(l) do { (void)(l); } while (0)

/* ------------------------------------------------------------ rwlock */

#define __LKPI_RW_WRITER 0x80000000u

static __always_inline void rwlock_init(rwlock_t *l) { l->cnts = 0; }
static __always_inline bool __lkpi_read_trylock(rwlock_t *l)
{
	unsigned int v = __atomic_load_n(&l->cnts, __ATOMIC_RELAXED);
	return !(v & __LKPI_RW_WRITER) &&
	       __atomic_compare_exchange_n(&l->cnts, &v, v + 1, false,
					   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static __always_inline bool __lkpi_write_trylock(rwlock_t *l)
{
	unsigned int v = 0;
	return __atomic_compare_exchange_n(&l->cnts, &v, __LKPI_RW_WRITER, false,
					   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static __always_inline void read_lock(rwlock_t *l)
{
	preempt_disable();
	while (!__lkpi_read_trylock(l))
		cpu_relax();
}
static __always_inline void read_unlock(rwlock_t *l)
{
	__atomic_fetch_sub(&l->cnts, 1, __ATOMIC_RELEASE);
	preempt_enable();
}
static __always_inline void write_lock(rwlock_t *l)
{
	preempt_disable();
	while (!__lkpi_write_trylock(l))
		cpu_relax();
}
static __always_inline void write_unlock(rwlock_t *l)
{
	__atomic_store_n(&l->cnts, 0, __ATOMIC_RELEASE);
	preempt_enable();
}
static __always_inline int read_trylock(rwlock_t *l)
{
	preempt_disable();
	if (__lkpi_read_trylock(l))
		return 1;
	preempt_enable();
	return 0;
}
static __always_inline int write_trylock(rwlock_t *l)
{
	preempt_disable();
	if (__lkpi_write_trylock(l))
		return 1;
	preempt_enable();
	return 0;
}
#define read_lock_irq(l) do { local_irq_disable(); read_lock(l); } while (0)
#define read_unlock_irq(l) do { read_unlock(l); local_irq_enable(); } while (0)
#define write_lock_irq(l) do { local_irq_disable(); write_lock(l); } while (0)
#define write_unlock_irq(l) do { write_unlock(l); local_irq_enable(); } while (0)
#define read_lock_irqsave(l, flags) do { local_irq_save(flags); read_lock(l); } while (0)
#define read_unlock_irqrestore(l, flags) do { read_unlock(l); local_irq_restore(flags); } while (0)
#define write_lock_irqsave(l, flags) do { local_irq_save(flags); write_lock(l); } while (0)
#define write_unlock_irqrestore(l, flags) do { write_unlock(l); local_irq_restore(flags); } while (0)
#define read_lock_bh(l) read_lock(l)
#define read_unlock_bh(l) read_unlock(l)
#define write_lock_bh(l) write_lock(l)
#define write_unlock_bh(l) write_unlock(l)
#define write_lock_nested(l, sub) write_lock(l)

/* -------------------------------------------- atomic_dec_and_lock */

/* Decrementa y, si llega a 0, devuelve 1 con el cerrojo cogido. */
static inline int __lkpi_atomic_dec_and_lock(atomic_t *atomic, spinlock_t *lock)
{
	if (atomic_add_unless(atomic, -1, 1))
		return 0;
	spin_lock(lock);
	if (atomic_dec_and_test(atomic))
		return 1;
	spin_unlock(lock);
	return 0;
}
static inline int __lkpi_atomic_dec_and_lock_irqsave(atomic_t *atomic, spinlock_t *lock,
						     unsigned long *flags)
{
	if (atomic_add_unless(atomic, -1, 1))
		return 0;
	spin_lock_irqsave(lock, *flags);
	if (atomic_dec_and_test(atomic))
		return 1;
	spin_unlock_irqrestore(lock, *flags);
	return 0;
}
#define atomic_dec_and_lock(a, l) __lkpi_atomic_dec_and_lock(a, l)
#define atomic_dec_and_lock_irqsave(a, l, flags) \
	__lkpi_atomic_dec_and_lock_irqsave(a, l, &(flags))

/* ------------------------------------------------------------ guardas */

DEFINE_LOCK_GUARD_1(raw_spinlock, raw_spinlock_t, raw_spin_lock(_T->lock), raw_spin_unlock(_T->lock))
DEFINE_LOCK_GUARD_1_COND(raw_spinlock, _try, raw_spin_trylock(_T->lock))
DEFINE_LOCK_GUARD_1(raw_spinlock_irq, raw_spinlock_t, raw_spin_lock_irq(_T->lock),
		    raw_spin_unlock_irq(_T->lock))
DEFINE_LOCK_GUARD_1(raw_spinlock_irqsave, raw_spinlock_t,
		    raw_spin_lock_irqsave(_T->lock, _T->flags),
		    raw_spin_unlock_irqrestore(_T->lock, _T->flags), unsigned long flags)
DEFINE_LOCK_GUARD_1(spinlock, spinlock_t, spin_lock(_T->lock), spin_unlock(_T->lock))
DEFINE_LOCK_GUARD_1_COND(spinlock, _try, spin_trylock(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irq, spinlock_t, spin_lock_irq(_T->lock), spin_unlock_irq(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_bh, spinlock_t, spin_lock_bh(_T->lock), spin_unlock_bh(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irqsave, spinlock_t, spin_lock_irqsave(_T->lock, _T->flags),
		    spin_unlock_irqrestore(_T->lock, _T->flags), unsigned long flags)
DEFINE_LOCK_GUARD_1(read_lock, rwlock_t, read_lock(_T->lock), read_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(write_lock, rwlock_t, write_lock(_T->lock), write_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(write_lock_irqsave, rwlock_t, write_lock_irqsave(_T->lock, _T->flags),
		    write_unlock_irqrestore(_T->lock, _T->flags), unsigned long flags)
DEFINE_LOCK_GUARD_0(irq, local_irq_disable(), local_irq_enable())
DEFINE_LOCK_GUARD_0(irqsave, local_irq_save(_T->flags), local_irq_restore(_T->flags),
		    unsigned long flags)
DEFINE_LOCK_GUARD_0(preempt, preempt_disable(), preempt_enable())

#endif
