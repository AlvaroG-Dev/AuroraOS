/* SPDX-License-Identifier: GPL-2.0-only */
/* Semaforo contador con esperas FIFO y concesion directa en up(). */
#ifndef _LKPI_LINUX_SEMAPHORE_H
#define _LKPI_LINUX_SEMAPHORE_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

struct semaphore {
	raw_spinlock_t lock;
	unsigned int count;
	struct list_head wait_list;
};

#define __SEMAPHORE_INITIALIZER(name, n) \
	{ .lock = __RAW_SPIN_LOCK_INITIALIZER((name).lock), .count = (n), \
	  .wait_list = LIST_HEAD_INIT((name).wait_list) }
/* Linux >= 6.4 exige el valor inicial; con un solo argumento vale 1. */
#define __lkpi_sem_def2(name, n) struct semaphore name = __SEMAPHORE_INITIALIZER(name, n)
#define __lkpi_sem_def1(name) __lkpi_sem_def2(name, 1)
#define __lkpi_sem_sel(_1, _2, NAME, ...) NAME
#define DEFINE_SEMAPHORE(...) \
	__lkpi_sem_sel(__VA_ARGS__, __lkpi_sem_def2, __lkpi_sem_def1)(__VA_ARGS__)

static inline void sema_init(struct semaphore *sem, int val)
{
	raw_spin_lock_init(&sem->lock);
	sem->count = val;
	INIT_LIST_HEAD(&sem->wait_list);
}

int lkpi_down_state_timeout(struct semaphore *sem, unsigned int state, long timeout);
int lkpi_down_trylock(struct semaphore *sem);
void lkpi_up(struct semaphore *sem);

#define down(sem) ((void)lkpi_down_state_timeout(sem, TASK_UNINTERRUPTIBLE, LONG_MAX))
#define down_interruptible(sem) lkpi_down_state_timeout(sem, TASK_INTERRUPTIBLE, LONG_MAX)
#define down_killable(sem) lkpi_down_state_timeout(sem, TASK_KILLABLE, LONG_MAX)
#define down_timeout(sem, jiffies) lkpi_down_state_timeout(sem, TASK_UNINTERRUPTIBLE, (long)(jiffies))
#define down_trylock(sem) lkpi_down_trylock(sem)
#define up(sem) lkpi_up(sem)

#endif
