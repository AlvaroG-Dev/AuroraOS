/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * refcount_t: contador que se satura en lugar de dar la vuelta. Un
 * incremento desde 0 (uso tras liberar) o un decremento por debajo de 0
 * avisan con WARN y dejan el contador saturado (el objeto se pierde, pero no
 * se libera dos veces).
 */
#ifndef _LKPI_LINUX_REFCOUNT_H
#define _LKPI_LINUX_REFCOUNT_H

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/limits.h>
#include <linux/spinlock.h>

struct mutex;

typedef struct refcount_struct {
	atomic_t refs;
} refcount_t;

#define REFCOUNT_INIT(n) { .refs = ATOMIC_INIT(n) }
#define REFCOUNT_MAX INT_MAX
#define REFCOUNT_SATURATED (INT_MIN / 2)

void lkpi_refcount_warn_saturate(refcount_t *r, int why);
#define __LKPI_RC_ADD_UAF 1
#define __LKPI_RC_ADD_OVF 2
#define __LKPI_RC_SUB_UAF 3

static inline void refcount_set(refcount_t *r, int n) { atomic_set(&r->refs, n); }
static inline void refcount_set_release(refcount_t *r, int n) { atomic_set_release(&r->refs, n); }
static inline unsigned int refcount_read(const refcount_t *r) { return atomic_read(&r->refs); }

static inline bool refcount_add_not_zero(int i, refcount_t *r)
{
	int old = refcount_read(r);
	do {
		if (!old)
			break;
	} while (!atomic_try_cmpxchg(&r->refs, &old, old + i));
	if (unlikely(old < 0 || old + i < 0))
		lkpi_refcount_warn_saturate(r, __LKPI_RC_ADD_OVF);
	return old;
}
static inline bool refcount_inc_not_zero(refcount_t *r) { return refcount_add_not_zero(1, r); }
#define refcount_inc_not_zero_acquire(r) refcount_inc_not_zero(r)
#define refcount_add_not_zero_acquire(i, r) refcount_add_not_zero(i, r)

static inline void refcount_add(int i, refcount_t *r)
{
	int old = atomic_fetch_add(i, &r->refs);
	if (unlikely(!old))
		lkpi_refcount_warn_saturate(r, __LKPI_RC_ADD_UAF);
	else if (unlikely(old < 0 || old + i < 0))
		lkpi_refcount_warn_saturate(r, __LKPI_RC_ADD_OVF);
}
static inline void refcount_inc(refcount_t *r) { refcount_add(1, r); }

static inline bool refcount_sub_and_test(int i, refcount_t *r)
{
	int old = atomic_fetch_sub(i, &r->refs);
	if (old == i)
		return true;
	if (unlikely(old < 0 || old - i < 0))
		lkpi_refcount_warn_saturate(r, __LKPI_RC_SUB_UAF);
	return false;
}
static inline bool refcount_dec_and_test(refcount_t *r) { return refcount_sub_and_test(1, r); }
static inline void refcount_dec(refcount_t *r)
{
	if (unlikely(atomic_fetch_sub(1, &r->refs) <= 1))
		lkpi_refcount_warn_saturate(r, __LKPI_RC_SUB_UAF);
}
static inline bool refcount_dec_if_one(refcount_t *r)
{
	int one = 1;
	return atomic_try_cmpxchg(&r->refs, &one, 0);
}
static inline bool refcount_dec_not_one(refcount_t *r)
{
	int old = refcount_read(r);
	do {
		if (unlikely(old == REFCOUNT_SATURATED))
			return true;
		if (old == 1)
			return false;
	} while (!atomic_try_cmpxchg(&r->refs, &old, old - 1));
	if (unlikely(old <= 0))
		lkpi_refcount_warn_saturate(r, __LKPI_RC_SUB_UAF);
	return true;
}
static inline bool refcount_dec_and_lock(refcount_t *r, spinlock_t *lock)
{
	if (refcount_dec_not_one(r))
		return false;
	spin_lock(lock);
	if (!refcount_dec_and_test(r)) {
		spin_unlock(lock);
		return false;
	}
	return true;
}
static inline bool __lkpi_refcount_dec_and_lock_irqsave(refcount_t *r, spinlock_t *lock,
							unsigned long *flags)
{
	if (refcount_dec_not_one(r))
		return false;
	spin_lock_irqsave(lock, *flags);
	if (!refcount_dec_and_test(r)) {
		spin_unlock_irqrestore(lock, *flags);
		return false;
	}
	return true;
}
#define refcount_dec_and_lock_irqsave(r, lock, flags) \
	__lkpi_refcount_dec_and_lock_irqsave(r, lock, flags)
bool lkpi_refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock);
#define refcount_dec_and_mutex_lock(r, lock) lkpi_refcount_dec_and_mutex_lock(r, lock)

#endif
