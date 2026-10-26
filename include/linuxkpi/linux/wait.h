/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Colas de espera de Linux (wait_queue_head) escritas en la capa: entradas
 * con funcion de despertar propia, esperas exclusivas, wake_up con clave
 * (poll) y la familia wait_event*(). Duermen con schedule() de la capa.
 */
#ifndef _LKPI_LINUX_WAIT_H
#define _LKPI_LINUX_WAIT_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/errno.h>

struct wait_queue_entry;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry, unsigned int mode,
				 int flags, void *key);

#define WQ_FLAG_EXCLUSIVE 0x01
#define WQ_FLAG_WOKEN 0x02
#define WQ_FLAG_CUSTOM 0x04
#define WQ_FLAG_DONE 0x08
#define WQ_FLAG_PRIORITY 0x10

struct wait_queue_entry {
	unsigned int flags;
	void *private;
	wait_queue_func_t func;
	struct list_head entry;
};
typedef struct wait_queue_entry wait_queue_entry_t;

struct wait_queue_head {
	spinlock_t lock;
	struct list_head head;
};
typedef struct wait_queue_head wait_queue_head_t;

int lkpi_default_wake_function(struct wait_queue_entry *e, unsigned int mode, int flags, void *key);
int lkpi_autoremove_wake_function(struct wait_queue_entry *e, unsigned int mode, int flags, void *key);
int lkpi_woken_wake_function(struct wait_queue_entry *e, unsigned int mode, int flags, void *key);
#define default_wake_function lkpi_default_wake_function
#define autoremove_wake_function lkpi_autoremove_wake_function
#define woken_wake_function lkpi_woken_wake_function

#define __WAIT_QUEUE_HEAD_INITIALIZER(name) \
	{ .lock = __SPIN_LOCK_INITIALIZER(name.lock), .head = LIST_HEAD_INIT(name.head) }
#define DECLARE_WAIT_QUEUE_HEAD(name) \
	struct wait_queue_head name = __WAIT_QUEUE_HEAD_INITIALIZER(name)
#define DECLARE_WAIT_QUEUE_HEAD_ONSTACK(name) DECLARE_WAIT_QUEUE_HEAD(name)

static inline void __lkpi_init_waitqueue_head(struct wait_queue_head *wq)
{
	spin_lock_init(&wq->lock);
	INIT_LIST_HEAD(&wq->head);
}
#define init_waitqueue_head(wq) __lkpi_init_waitqueue_head(wq)

#define __WAITQUEUE_INITIALIZER(name, tsk) \
	{ .private = (tsk), .func = default_wake_function, .entry = { NULL, NULL } }
#define DECLARE_WAITQUEUE(name, tsk) \
	struct wait_queue_entry name = __WAITQUEUE_INITIALIZER(name, tsk)

static inline void init_waitqueue_entry(struct wait_queue_entry *e, struct task_struct *p)
{
	e->flags = 0;
	e->private = p;
	e->func = default_wake_function;
}
static inline void init_waitqueue_func_entry(struct wait_queue_entry *e, wait_queue_func_t func)
{
	e->flags = 0;
	e->private = NULL;
	e->func = func;
}

static inline int waitqueue_active(struct wait_queue_head *wq) { return !list_empty(&wq->head); }
static inline bool wq_has_single_sleeper(struct wait_queue_head *wq)
{
	return list_is_singular(&wq->head);
}
static inline bool wq_has_sleeper(struct wait_queue_head *wq)
{
	smp_mb();
	return waitqueue_active(wq);
}

/* Con el cerrojo de la cola cogido. */
static inline void __add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	list_add(&e->entry, &wq->head);
}
static inline void __add_wait_queue_entry_tail(struct wait_queue_head *wq,
					       struct wait_queue_entry *e)
{
	list_add_tail(&e->entry, &wq->head);
}
static inline void __add_wait_queue_exclusive(struct wait_queue_head *wq,
					      struct wait_queue_entry *e)
{
	e->flags |= WQ_FLAG_EXCLUSIVE;
	__add_wait_queue(wq, e);
}
static inline void __remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	(void)wq;
	list_del(&e->entry);
}

void lkpi_add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void lkpi_add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e);
void lkpi_add_wait_queue_priority(struct wait_queue_head *wq, struct wait_queue_entry *e);
void lkpi_remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
int lkpi_wake_up(struct wait_queue_head *wq, unsigned int mode, int nr, void *key);
int lkpi_wake_up_locked_key(struct wait_queue_head *wq, unsigned int mode, int nr, void *key);
void lkpi_prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
bool lkpi_prepare_to_wait_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e,
				    int state);
long lkpi_prepare_to_wait_event(struct wait_queue_head *wq, struct wait_queue_entry *e,
				int state);
void lkpi_finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e);
void lkpi_init_wait_entry(struct wait_queue_entry *e, int flags);
long lkpi_wait_woken(struct wait_queue_entry *e, unsigned int mode, long timeout);

#define add_wait_queue(wq, e) lkpi_add_wait_queue(wq, e)
#define add_wait_queue_exclusive(wq, e) lkpi_add_wait_queue_exclusive(wq, e)
#define add_wait_queue_priority(wq, e) lkpi_add_wait_queue_priority(wq, e)
#define remove_wait_queue(wq, e) lkpi_remove_wait_queue(wq, e)
#define prepare_to_wait(wq, e, s) lkpi_prepare_to_wait(wq, e, s)
#define prepare_to_wait_exclusive(wq, e, s) lkpi_prepare_to_wait_exclusive(wq, e, s)
#define prepare_to_wait_event(wq, e, s) lkpi_prepare_to_wait_event(wq, e, s)
#define finish_wait(wq, e) lkpi_finish_wait(wq, e)
#define init_wait_entry(e, f) lkpi_init_wait_entry(e, f)
#define wait_woken(e, m, t) lkpi_wait_woken(e, m, t)

#define DEFINE_WAIT_FUNC(name, function) \
	struct wait_queue_entry name = { \
		.private = current, .func = function, \
		.entry = LIST_HEAD_INIT((name).entry) }
#define DEFINE_WAIT(name) DEFINE_WAIT_FUNC(name, autoremove_wake_function)
#define init_wait(e) \
	do { (e)->private = current; (e)->func = autoremove_wake_function; \
	     INIT_LIST_HEAD(&(e)->entry); (e)->flags = 0; } while (0)
#define init_wait_func(e, f) \
	do { (e)->private = current; (e)->func = (f); \
	     INIT_LIST_HEAD(&(e)->entry); (e)->flags = 0; } while (0)

/* ------------------------------------------------------------ wake_up */

#define poll_to_key(m) ((void *)(uintptr_t)(m))
#define key_to_poll(k) ((__force __poll_t)(uintptr_t)(k))

#define __wake_up(wq, mode, nr, key) lkpi_wake_up(wq, mode, nr, key)
#define __wake_up_locked_key(wq, mode, key) lkpi_wake_up_locked_key(wq, mode, 0, key)
#define __wake_up_locked(wq, mode, nr) lkpi_wake_up_locked_key(wq, mode, nr, NULL)
#define wake_up(x) lkpi_wake_up(x, TASK_NORMAL, 1, NULL)
#define wake_up_nr(x, nr) lkpi_wake_up(x, TASK_NORMAL, nr, NULL)
#define wake_up_all(x) lkpi_wake_up(x, TASK_NORMAL, 0, NULL)
#define wake_up_locked(x) lkpi_wake_up_locked_key(x, TASK_NORMAL, 1, NULL)
#define wake_up_all_locked(x) lkpi_wake_up_locked_key(x, TASK_NORMAL, 0, NULL)
#define wake_up_interruptible(x) lkpi_wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define wake_up_interruptible_nr(x, nr) lkpi_wake_up(x, TASK_INTERRUPTIBLE, nr, NULL)
#define wake_up_interruptible_all(x) lkpi_wake_up(x, TASK_INTERRUPTIBLE, 0, NULL)
#define wake_up_interruptible_sync(x) wake_up_interruptible(x)
#define wake_up_sync(x) wake_up(x)
#define wake_up_poll(x, m) lkpi_wake_up(x, TASK_NORMAL, 1, poll_to_key(m))
#define wake_up_poll_on_current_cpu(x, m) wake_up_poll(x, m)
#define wake_up_locked_poll(x, m) lkpi_wake_up_locked_key(x, TASK_NORMAL, 0, poll_to_key(m))
#define wake_up_interruptible_poll(x, m) lkpi_wake_up(x, TASK_INTERRUPTIBLE, 1, poll_to_key(m))
#define wake_up_interruptible_sync_poll(x, m) wake_up_interruptible_poll(x, m)
#define wake_up_interruptible_sync_poll_locked(x, m) \
	lkpi_wake_up_locked_key(x, TASK_INTERRUPTIBLE, 1, poll_to_key(m))
#define wake_up_pollfree(x) wake_up_poll(x, 0)

/* -------------------------------------------------------- wait_event */

/*
 * Bucle comun: entra en la cola en `state`, comprueba `cond` y duerme con
 * `sleep`. Devuelve `init_ret` (o lo que `sleep` deje en __ret), o el error
 * de prepare_to_wait_event si una senal interrumpe la espera.
 */
#define __lkpi_wait_event(wqh, cond, state, excl, init_ret, sleep) \
({ \
	struct wait_queue_entry __we; \
	long __ret = (init_ret), __sig; \
	init_wait_entry(&__we, (excl) ? WQ_FLAG_EXCLUSIVE : 0); \
	for (;;) { \
		__sig = prepare_to_wait_event(&(wqh), &__we, (state)); \
		if (cond) \
			break; \
		if (__sig) { \
			__ret = __sig; \
			break; \
		} \
		sleep; \
	} \
	finish_wait(&(wqh), &__we); \
	__ret; \
})

/* Condicion de las esperas con plazo (usa el __ret del bucle comun). */
#define __lkpi_cond_tmo(cond) \
	({ bool __c = (cond); if (__c && !__ret) __ret = 1; __c || !__ret; })

#define __lkpi_wait_plain(wq, cond, state, excl) \
	({ long __r = 0; \
	   if (!(cond)) \
		__r = __lkpi_wait_event(wq, cond, state, excl, 0, schedule()); \
	   (int)__r; })

#define __lkpi_wait_tmo(wq, cond, state, excl, tmo) \
	({ long __ret = (tmo); \
	   if (!__lkpi_cond_tmo(cond)) \
		__ret = __lkpi_wait_event(wq, __lkpi_cond_tmo(cond), state, excl, tmo, \
					  __ret = schedule_timeout(__ret)); \
	   __ret; })

#define wait_event(wq, cond) ((void)__lkpi_wait_plain(wq, cond, TASK_UNINTERRUPTIBLE, 0))
#define wait_event_exclusive(wq, cond) ((void)__lkpi_wait_plain(wq, cond, TASK_UNINTERRUPTIBLE, 1))
#define wait_event_idle(wq, cond) ((void)__lkpi_wait_plain(wq, cond, TASK_IDLE, 0))
#define wait_event_idle_exclusive(wq, cond) ((void)__lkpi_wait_plain(wq, cond, TASK_IDLE, 1))
#define wait_event_freezable(wq, cond) __lkpi_wait_plain(wq, cond, TASK_INTERRUPTIBLE, 0)
#define wait_event_interruptible(wq, cond) __lkpi_wait_plain(wq, cond, TASK_INTERRUPTIBLE, 0)
#define wait_event_interruptible_exclusive(wq, cond) \
	__lkpi_wait_plain(wq, cond, TASK_INTERRUPTIBLE, 1)
#define wait_event_killable(wq, cond) __lkpi_wait_plain(wq, cond, TASK_KILLABLE, 0)
#define wait_event_killable_exclusive(wq, cond) __lkpi_wait_plain(wq, cond, TASK_KILLABLE, 1)
#define wait_event_state(wq, cond, state) __lkpi_wait_plain(wq, cond, state, 0)
#define io_wait_event(wq, cond) wait_event(wq, cond)

#define wait_event_timeout(wq, cond, tmo) \
	__lkpi_wait_tmo(wq, cond, TASK_UNINTERRUPTIBLE, 0, tmo)
#define wait_event_idle_timeout(wq, cond, tmo) __lkpi_wait_tmo(wq, cond, TASK_IDLE, 0, tmo)
#define wait_event_idle_exclusive_timeout(wq, cond, tmo) \
	__lkpi_wait_tmo(wq, cond, TASK_IDLE, 1, tmo)
#define wait_event_interruptible_timeout(wq, cond, tmo) \
	__lkpi_wait_tmo(wq, cond, TASK_INTERRUPTIBLE, 0, tmo)
#define wait_event_freezable_timeout(wq, cond, tmo) wait_event_interruptible_timeout(wq, cond, tmo)
#define wait_event_killable_timeout(wq, cond, tmo) __lkpi_wait_tmo(wq, cond, TASK_KILLABLE, 0, tmo)

#define wait_event_cmd(wq, cond, cmd1, cmd2) \
	do { if (!(cond)) \
		(void)__lkpi_wait_event(wq, cond, TASK_UNINTERRUPTIBLE, 0, 0, \
					cmd1; schedule(); cmd2); } while (0)
#define wait_event_interruptible_cmd(wq, cond, cmd1, cmd2) \
	({ long __r = 0; if (!(cond)) \
		__r = __lkpi_wait_event(wq, cond, TASK_INTERRUPTIBLE, 0, 0, \
					cmd1; schedule(); cmd2); (int)__r; })

/* Con wq.lock cogido (spin_lock_irq) al entrar y al salir. */
#define __lkpi_wait_lock_irq(wq, cond, state, lock, cmd) \
	({ long __r = 0; if (!(cond)) \
		__r = __lkpi_wait_event(wq, cond, state, 0, 0, \
			spin_unlock_irq(&(lock)); cmd; schedule(); spin_lock_irq(&(lock))); \
	   (int)__r; })
#define wait_event_lock_irq(wq, cond, lock) \
	((void)__lkpi_wait_lock_irq(wq, cond, TASK_UNINTERRUPTIBLE, lock, ))
#define wait_event_lock_irq_cmd(wq, cond, lock, cmd) \
	((void)__lkpi_wait_lock_irq(wq, cond, TASK_UNINTERRUPTIBLE, lock, cmd))
#define wait_event_interruptible_lock_irq(wq, cond, lock) \
	__lkpi_wait_lock_irq(wq, cond, TASK_INTERRUPTIBLE, lock, )
#define wait_event_interruptible_lock_irq_cmd(wq, cond, lock, cmd) \
	__lkpi_wait_lock_irq(wq, cond, TASK_INTERRUPTIBLE, lock, cmd)
#define wait_event_lock_irq_timeout(wq, cond, lock, tmo) \
	({ long __ret = (tmo); \
	   if (!__lkpi_cond_tmo(cond)) \
		__ret = __lkpi_wait_event(wq, __lkpi_cond_tmo(cond), TASK_UNINTERRUPTIBLE, 0, \
			tmo, spin_unlock_irq(&(lock)); __ret = schedule_timeout(__ret); \
			spin_lock_irq(&(lock))); \
	   __ret; })
#define wait_event_interruptible_lock_irq_timeout(wq, cond, lock, tmo) \
	({ long __ret = (tmo); \
	   if (!__lkpi_cond_tmo(cond)) \
		__ret = __lkpi_wait_event(wq, __lkpi_cond_tmo(cond), TASK_INTERRUPTIBLE, 0, \
			tmo, spin_unlock_irq(&(lock)); __ret = schedule_timeout(__ret); \
			spin_lock_irq(&(lock))); \
	   __ret; })

#endif
