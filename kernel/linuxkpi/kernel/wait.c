// SPDX-License-Identifier: GPL-2.0-only
/* Colas de espera de Linux, completion y colas compartidas por direccion. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/completion.h>
#include <linux/hash.h>
#include <linux/limits.h>
#include <linux/wait.h>
#include <linux/wait_bit.h>

#include "../lkpi_internal.h"

/* ------------------------------------------------- funciones de despertar */

int lkpi_default_wake_function(struct wait_queue_entry *e, unsigned int mode, int flags, void *key)
{
	(void)flags;
	(void)key;
	return lkpi_wake_up_state(e->private, mode);
}
EXPORT_SYMBOL(lkpi_default_wake_function);

int lkpi_autoremove_wake_function(struct wait_queue_entry *e, unsigned int mode, int flags,
				  void *key)
{
	int ret = lkpi_default_wake_function(e, mode, flags, key);

	if (ret) {
		/* finish_wait() mira la entrada sin cerrojo (list_empty_careful). */
		list_del_init(&e->entry);
		smp_wmb();
	}
	return ret;
}
EXPORT_SYMBOL(lkpi_autoremove_wake_function);

int lkpi_woken_wake_function(struct wait_queue_entry *e, unsigned int mode, int flags, void *key)
{
	/* Pareja del smp_store_mb de wait_woken(). */
	smp_mb();
	e->flags |= WQ_FLAG_WOKEN;
	return lkpi_default_wake_function(e, mode, flags, key);
}
EXPORT_SYMBOL(lkpi_woken_wake_function);

/* ------------------------------------------------------- alta y baja */

void lkpi_add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;

	e->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq->lock, flags);
	__add_wait_queue(wq, e);
	spin_unlock_irqrestore(&wq->lock, flags);
}
EXPORT_SYMBOL(lkpi_add_wait_queue);

void lkpi_add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;

	e->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq->lock, flags);
	__add_wait_queue_entry_tail(wq, e);
	spin_unlock_irqrestore(&wq->lock, flags);
}
EXPORT_SYMBOL(lkpi_add_wait_queue_exclusive);

/* Prioritaria: primera de la cola y exclusiva (se queda con el despertar). */
void lkpi_add_wait_queue_priority(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;

	e->flags |= WQ_FLAG_EXCLUSIVE | WQ_FLAG_PRIORITY;
	spin_lock_irqsave(&wq->lock, flags);
	__add_wait_queue(wq, e);
	spin_unlock_irqrestore(&wq->lock, flags);
}
EXPORT_SYMBOL(lkpi_add_wait_queue_priority);

void lkpi_remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;

	spin_lock_irqsave(&wq->lock, flags);
	__remove_wait_queue(wq, e);
	spin_unlock_irqrestore(&wq->lock, flags);
}
EXPORT_SYMBOL(lkpi_remove_wait_queue);

/* ------------------------------------------------------------ wake_up */

/*
 * Recorre la cola llamando a la funcion de cada entrada. Las no exclusivas
 * se despiertan siempre; tras despertar `nr` exclusivas se para (nr == 0:
 * todas). Devuelve cuantas exclusivas desperto.
 */
static int __lkpi_wake_up_common(struct wait_queue_head *wq, unsigned int mode, int nr,
				 void *key)
{
	struct wait_queue_entry *curr, *next;
	int woken = 0;

	list_for_each_entry_safe(curr, next, &wq->head, entry) {
		unsigned int flags = curr->flags;
		int ret = curr->func(curr, mode, 0, key);

		if (ret < 0)
			break;
		if (ret && (flags & WQ_FLAG_EXCLUSIVE)) {
			woken++;
			if (nr && woken >= nr)
				break;
		}
	}
	return woken;
}

int lkpi_wake_up(struct wait_queue_head *wq, unsigned int mode, int nr, void *key)
{
	unsigned long flags;
	int woken;

	spin_lock_irqsave(&wq->lock, flags);
	woken = __lkpi_wake_up_common(wq, mode, nr, key);
	spin_unlock_irqrestore(&wq->lock, flags);
	return woken;
}
EXPORT_SYMBOL(lkpi_wake_up);

int lkpi_wake_up_locked_key(struct wait_queue_head *wq, unsigned int mode, int nr, void *key)
{
	return __lkpi_wake_up_common(wq, mode, nr, key);
}
EXPORT_SYMBOL(lkpi_wake_up_locked_key);

/* ------------------------------------------------------------ esperas */

void lkpi_init_wait_entry(struct wait_queue_entry *e, int flags)
{
	e->flags = flags;
	e->private = current;
	e->func = autoremove_wake_function;
	INIT_LIST_HEAD(&e->entry);
}
EXPORT_SYMBOL(lkpi_init_wait_entry);

void lkpi_prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
	unsigned long flags;

	e->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq->lock, flags);
	if (list_empty(&e->entry))
		__add_wait_queue(wq, e);
	set_current_state(state);
	spin_unlock_irqrestore(&wq->lock, flags);
}
EXPORT_SYMBOL(lkpi_prepare_to_wait);

bool lkpi_prepare_to_wait_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e,
				    int state)
{
	unsigned long flags;
	bool was_empty = false;

	e->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq->lock, flags);
	if (list_empty(&e->entry)) {
		was_empty = list_empty(&wq->head);
		__add_wait_queue_entry_tail(wq, e);
	}
	set_current_state(state);
	spin_unlock_irqrestore(&wq->lock, flags);
	return was_empty;
}
EXPORT_SYMBOL(lkpi_prepare_to_wait_exclusive);

long lkpi_prepare_to_wait_event(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
	unsigned long flags;
	long ret = 0;

	spin_lock_irqsave(&wq->lock, flags);
	if (signal_pending_state(state, current)) {
		/* Sale sin dormir; la entrada fuera para no robar un despertar. */
		list_del_init(&e->entry);
		ret = -ERESTARTSYS;
	} else {
		if (list_empty(&e->entry)) {
			if (e->flags & WQ_FLAG_EXCLUSIVE)
				__add_wait_queue_entry_tail(wq, e);
			else
				__add_wait_queue(wq, e);
		}
		set_current_state(state);
	}
	spin_unlock_irqrestore(&wq->lock, flags);
	return ret;
}
EXPORT_SYMBOL(lkpi_prepare_to_wait_event);

void lkpi_finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;

	__set_current_state(TASK_RUNNING);
	if (!list_empty_careful(&e->entry)) {
		spin_lock_irqsave(&wq->lock, flags);
		list_del_init(&e->entry);
		spin_unlock_irqrestore(&wq->lock, flags);
	}
}
EXPORT_SYMBOL(lkpi_finish_wait);

long lkpi_wait_woken(struct wait_queue_entry *e, unsigned int mode, long timeout)
{
	/* WOKEN se lee despues de fijar el estado: no se pierde un despertar. */
	set_current_state(mode);
	if (!(e->flags & WQ_FLAG_WOKEN) && !signal_pending_state(mode, current))
		timeout = schedule_timeout(timeout);
	__set_current_state(TASK_RUNNING);
	smp_store_mb(e->flags, e->flags & ~WQ_FLAG_WOKEN);
	return timeout;
}
EXPORT_SYMBOL(lkpi_wait_woken);

/* --------------------------------------------------------- completion */

void lkpi_complete(struct completion *x)
{
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	if (x->done != UINT_MAX)
		x->done++;
	__lkpi_wake_up_common(&x->wait, TASK_NORMAL, 1, NULL);
	spin_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(lkpi_complete);

void lkpi_complete_all(struct completion *x)
{
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	x->done = UINT_MAX;
	__lkpi_wake_up_common(&x->wait, TASK_NORMAL, 0, NULL);
	spin_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(lkpi_complete_all);

/*
 * Espera exclusiva bajo x->wait.lock: comprobar `done` y consumirlo es
 * atomico respecto a complete(). Devuelve el tiempo restante (>= 1), 0 si
 * vencio el plazo o -ERESTARTSYS.
 */
long lkpi_wait_for_completion_state_timeout(struct completion *x, unsigned int state,
					    long timeout)
{
	struct wait_queue_entry wait;
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	if (!x->done) {
		init_waitqueue_entry(&wait, current);
		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_entry_tail(&x->wait, &wait);
		do {
			if (signal_pending_state(state, current)) {
				timeout = -ERESTARTSYS;
				break;
			}
			__set_current_state(state);
			spin_unlock_irqrestore(&x->wait.lock, flags);
			timeout = schedule_timeout(timeout);
			spin_lock_irqsave(&x->wait.lock, flags);
		} while (!x->done && timeout);
		__remove_wait_queue(&x->wait, &wait);
		if (!x->done) {
			spin_unlock_irqrestore(&x->wait.lock, flags);
			return timeout;
		}
	}
	if (x->done != UINT_MAX)
		x->done--;
	spin_unlock_irqrestore(&x->wait.lock, flags);
	return timeout > 0 ? timeout : 1;
}
EXPORT_SYMBOL(lkpi_wait_for_completion_state_timeout);

bool lkpi_try_wait_for_completion(struct completion *x)
{
	unsigned long flags;
	bool ret = false;

	if (!READ_ONCE(x->done))
		return false;
	spin_lock_irqsave(&x->wait.lock, flags);
	if (x->done) {
		if (x->done != UINT_MAX)
			x->done--;
		ret = true;
	}
	spin_unlock_irqrestore(&x->wait.lock, flags);
	return ret;
}
EXPORT_SYMBOL(lkpi_try_wait_for_completion);

bool lkpi_completion_done(struct completion *x)
{
	unsigned long flags;

	if (!READ_ONCE(x->done))
		return false;
	/* Espera a que complete() suelte el cerrojo (x puede liberarse luego). */
	spin_lock_irqsave(&x->wait.lock, flags);
	spin_unlock_irqrestore(&x->wait.lock, flags);
	return true;
}
EXPORT_SYMBOL(lkpi_completion_done);

/* ---------------------------------------------- colas por direccion */

#define LKPI_BIT_WQ_BITS 8
static struct wait_queue_head lkpi_bit_wq[1 << LKPI_BIT_WQ_BITS];

struct wait_queue_head *lkpi_bit_waitqueue(const void *addr)
{
	return &lkpi_bit_wq[hash_ptr(addr, LKPI_BIT_WQ_BITS)];
}
EXPORT_SYMBOL(lkpi_bit_waitqueue);

void lkpi_wait_init(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lkpi_bit_wq); i++)
		init_waitqueue_head(&lkpi_bit_wq[i]);
}
