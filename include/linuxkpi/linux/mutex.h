/* SPDX-License-Identifier: GPL-2.0-only */
/* mutex de Linux (propietario, esperas FIFO con traspaso directo). */
#ifndef _LKPI_LINUX_MUTEX_H
#define _LKPI_LINUX_MUTEX_H

#include <linux/mutex_types.h>
#include <linux/spinlock.h>
#include <linux/lockdep.h>
#include <linux/cleanup.h>

#define __MUTEX_INITIALIZER(name) \
	{ .owner = ATOMIC_LONG_INIT(0), .wait_lock = __RAW_SPIN_LOCK_INITIALIZER(name.wait_lock), \
	  .wait_list = LIST_HEAD_INIT(name.wait_list) }
#define DEFINE_MUTEX(name) struct mutex name = __MUTEX_INITIALIZER(name)

static inline void __lkpi_mutex_init(struct mutex *m)
{
	atomic_long_set(&m->owner, 0);
	raw_spin_lock_init(&m->wait_lock);
	INIT_LIST_HEAD(&m->wait_list);
}
#define mutex_init(m) __lkpi_mutex_init(m)
#define __mutex_init(m, name, key) __lkpi_mutex_init(m)
#define mutex_init_with_key(m, key) __lkpi_mutex_init(m)
#define mutex_destroy(m) do { (void)(m); } while (0)
#define devm_mutex_init(dev, m) ({ (void)(dev); __lkpi_mutex_init(m); 0; })

void lkpi_mutex_lock(struct mutex *m);
int lkpi_mutex_lock_interruptible(struct mutex *m);
int lkpi_mutex_lock_killable(struct mutex *m);
int lkpi_mutex_trylock(struct mutex *m);
void lkpi_mutex_unlock(struct mutex *m);
struct task_struct *lkpi_mutex_owner(struct mutex *m);

#define mutex_lock(m) lkpi_mutex_lock(m)
#define mutex_lock_interruptible(m) lkpi_mutex_lock_interruptible(m)
#define mutex_lock_killable(m) lkpi_mutex_lock_killable(m)
#define mutex_lock_io(m) lkpi_mutex_lock(m)
#define mutex_trylock(m) lkpi_mutex_trylock(m)
#define mutex_unlock(m) lkpi_mutex_unlock(m)
#define mutex_lock_nested(m, sub) lkpi_mutex_lock(m)
#define mutex_lock_nest_lock(m, nl) lkpi_mutex_lock(m)
#define mutex_lock_io_nested(m, sub) lkpi_mutex_lock(m)
#define mutex_lock_interruptible_nested(m, sub) lkpi_mutex_lock_interruptible(m)
#define mutex_lock_killable_nested(m, sub) lkpi_mutex_lock_killable(m)
#define mutex_trylock_nested(m, sub) lkpi_mutex_trylock(m)
#define mutex_is_locked(m) (atomic_long_read(&(m)->owner) != 0)
#define mutex_get_owner(m) ((unsigned long)lkpi_mutex_owner(m))

/* Decrementa; si llega a 0 devuelve 1 con el mutex cogido. */
static inline int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		mutex_unlock(lock);
		return 0;
	}
	return 1;
}

DEFINE_GUARD(mutex, struct mutex *, mutex_lock(_T), mutex_unlock(_T))
DEFINE_GUARD_COND(mutex, _try, mutex_trylock(_T))
DEFINE_GUARD_COND(mutex, _intr, mutex_lock_interruptible(_T) == 0)
DEFINE_FREE(mutex, struct mutex *, if (_T) mutex_unlock(_T))

#endif
