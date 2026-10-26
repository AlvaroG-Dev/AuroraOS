/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Semaforo de lectura/escritura. count: > 0 lectores, -1 escritor, 0 libre.
 * Las esperas son FIFO y se conceden directamente al soltar (un escritor o
 * todos los lectores consecutivos de la cabeza); un lector nuevo no adelanta
 * a un escritor que ya espera.
 */
#ifndef _LKPI_LINUX_RWSEM_H
#define _LKPI_LINUX_RWSEM_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/lockdep.h>
#include <linux/cleanup.h>

struct rw_semaphore {
	long count;
	raw_spinlock_t wait_lock;
	struct list_head wait_list;
};

#define __RWSEM_INITIALIZER(name) \
	{ .count = 0, .wait_lock = __RAW_SPIN_LOCK_INITIALIZER(name.wait_lock), \
	  .wait_list = LIST_HEAD_INIT((name).wait_list) }
#define DECLARE_RWSEM(name) struct rw_semaphore name = __RWSEM_INITIALIZER(name)

static inline void __lkpi_init_rwsem(struct rw_semaphore *s)
{
	s->count = 0;
	raw_spin_lock_init(&s->wait_lock);
	INIT_LIST_HEAD(&s->wait_list);
}
#define init_rwsem(s) __lkpi_init_rwsem(s)
#define __init_rwsem(s, name, key) __lkpi_init_rwsem(s)

int lkpi_down_read_state(struct rw_semaphore *s, unsigned int state);
int lkpi_down_write_state(struct rw_semaphore *s, unsigned int state);
int lkpi_down_read_trylock(struct rw_semaphore *s);
int lkpi_down_write_trylock(struct rw_semaphore *s);
void lkpi_up_read(struct rw_semaphore *s);
void lkpi_up_write(struct rw_semaphore *s);
void lkpi_downgrade_write(struct rw_semaphore *s);

#define down_read(s) ((void)lkpi_down_read_state(s, TASK_UNINTERRUPTIBLE))
#define down_read_interruptible(s) lkpi_down_read_state(s, TASK_INTERRUPTIBLE)
#define down_read_killable(s) lkpi_down_read_state(s, TASK_KILLABLE)
#define down_read_trylock(s) lkpi_down_read_trylock(s)
#define down_read_nested(s, sub) down_read(s)
#define down_read_non_owner(s) down_read(s)
#define up_read(s) lkpi_up_read(s)
#define up_read_non_owner(s) lkpi_up_read(s)
#define down_write(s) ((void)lkpi_down_write_state(s, TASK_UNINTERRUPTIBLE))
#define down_write_killable(s) lkpi_down_write_state(s, TASK_KILLABLE)
#define down_write_trylock(s) lkpi_down_write_trylock(s)
#define down_write_nested(s, sub) down_write(s)
#define down_write_nest_lock(s, nl) down_write(s)
#define down_write_killable_nested(s, sub) down_write_killable(s)
#define up_write(s) lkpi_up_write(s)
#define downgrade_write(s) lkpi_downgrade_write(s)
#define rwsem_is_locked(s) (READ_ONCE((s)->count) != 0)
#define rwsem_is_contended(s) (!list_empty(&(s)->wait_list))
#define rwsem_assert_held(s) do { (void)(s); } while (0)
#define rwsem_assert_held_write(s) do { (void)(s); } while (0)
#define rwsem_assert_held_nolockdep(s) do { (void)(s); } while (0)
#define rwsem_assert_held_write_nolockdep(s) do { (void)(s); } while (0)

DEFINE_GUARD(rwsem_read, struct rw_semaphore *, down_read(_T), up_read(_T))
DEFINE_GUARD_COND(rwsem_read, _try, down_read_trylock(_T))
DEFINE_GUARD_COND(rwsem_read, _intr, down_read_interruptible(_T) == 0)
DEFINE_GUARD(rwsem_write, struct rw_semaphore *, down_write(_T), up_write(_T))
DEFINE_GUARD_COND(rwsem_write, _try, down_write_trylock(_T))
DEFINE_FREE(up_read, struct rw_semaphore *, if (_T) up_read(_T))
DEFINE_FREE(up_write, struct rw_semaphore *, if (_T) up_write(_T))

#endif
