// SPDX-License-Identifier: GPL-2.0-only
/*
 * kthread_worker sobre los kthreads de la capa. Un hilo por worker; el
 * trabajo se encola en work_list y el hilo lo ejecuta en orden. Los trabajos
 * retardados usan un timer_list que los encola al vencer.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/kthread.h>
#include <linux/slab.h>

static int lkpi_kworker_fn(void *arg)
{
	struct kthread_worker *w = arg;
	unsigned long flags;

	for (;;) {
		struct kthread_work *work;

		wait_event(w->wake_wq, !list_empty(&w->work_list) || w->stopping);
		raw_spin_lock_irqsave(&w->lock, flags);
		if (list_empty(&w->work_list)) {
			bool stop = w->stopping;

			raw_spin_unlock_irqrestore(&w->lock, flags);
			if (stop)
				break;
			continue;
		}
		work = list_first_entry(&w->work_list, struct kthread_work, node);
		list_del_init(&work->node);
		w->current_work = work;
		raw_spin_unlock_irqrestore(&w->lock, flags);

		work->func(work);

		raw_spin_lock_irqsave(&w->lock, flags);
		w->current_work = NULL;
		raw_spin_unlock_irqrestore(&w->lock, flags);
		wake_up_all(&w->idle_wq);
	}
	return 0;
}

struct kthread_worker *lkpi_kthread_create_worker(unsigned int flags, const char *namefmt, ...)
{
	struct kthread_worker *w;
	va_list ap;
	char name[TASK_COMM_LEN];

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return ERR_PTR(-ENOMEM);
	kthread_init_worker(w);
	w->flags = flags;
	va_start(ap, namefmt);
	vsnprintf(name, sizeof(name), namefmt, ap);
	va_end(ap);
	w->task = kthread_run(lkpi_kworker_fn, w, "%s", name);
	if (IS_ERR(w->task)) {
		long err = PTR_ERR(w->task);

		kfree(w);
		return ERR_PTR(err);
	}
	return w;
}
EXPORT_SYMBOL(lkpi_kthread_create_worker);

/* Con el lock tomado */
static bool lkpi_kwork_queue_locked(struct kthread_worker *w, struct kthread_work *work)
{
	if (!list_empty(&work->node) || work->canceling)
		return false;
	work->worker = w;
	list_add_tail(&work->node, &w->work_list);
	wake_up(&w->wake_wq);
	return true;
}

bool lkpi_kthread_queue_work(struct kthread_worker *w, struct kthread_work *work)
{
	unsigned long flags;
	bool ret;

	raw_spin_lock_irqsave(&w->lock, flags);
	ret = lkpi_kwork_queue_locked(w, work);
	raw_spin_unlock_irqrestore(&w->lock, flags);
	return ret;
}
EXPORT_SYMBOL(lkpi_kthread_queue_work);

void lkpi_kthread_delayed_work_timer_fn(struct timer_list *t)
{
	struct kthread_delayed_work *dwork = from_timer(dwork, t, timer);
	struct kthread_work *work = &dwork->work;
	struct kthread_worker *w = work->worker;
	unsigned long flags;

	if (!w)
		return;
	raw_spin_lock_irqsave(&w->lock, flags);
	list_del_init(&work->node);	/* salia de delayed_work_list */
	lkpi_kwork_queue_locked(w, work);
	raw_spin_unlock_irqrestore(&w->lock, flags);
}
EXPORT_SYMBOL(lkpi_kthread_delayed_work_timer_fn);

bool lkpi_kthread_queue_delayed_work(struct kthread_worker *w, struct kthread_delayed_work *dwork,
				     unsigned long delay)
{
	struct kthread_work *work = &dwork->work;
	unsigned long flags;
	bool ret = false;

	if (!delay)
		return lkpi_kthread_queue_work(w, work);
	raw_spin_lock_irqsave(&w->lock, flags);
	if (list_empty(&work->node) && !work->canceling) {
		work->worker = w;
		list_add_tail(&work->node, &w->delayed_work_list);
		mod_timer(&dwork->timer, jiffies + delay);
		ret = true;
	}
	raw_spin_unlock_irqrestore(&w->lock, flags);
	return ret;
}
EXPORT_SYMBOL(lkpi_kthread_queue_delayed_work);

/* Quita el trabajo de cualquier lista (cancela el timer si estaba retardado) */
static bool lkpi_kwork_dequeue(struct kthread_work *work, bool delayed)
{
	struct kthread_worker *w = work->worker;
	unsigned long flags;
	bool was = false;

	if (!w)
		return false;
	if (delayed)
		del_timer_sync(&to_kthread_delayed_work(work)->timer);
	raw_spin_lock_irqsave(&w->lock, flags);
	if (!list_empty(&work->node)) {
		list_del_init(&work->node);
		was = true;
	}
	raw_spin_unlock_irqrestore(&w->lock, flags);
	return was;
}

bool lkpi_kthread_mod_delayed_work(struct kthread_worker *w, struct kthread_delayed_work *dwork,
				   unsigned long delay)
{
	bool was = lkpi_kwork_dequeue(&dwork->work, true);

	lkpi_kthread_queue_delayed_work(w, dwork, delay);
	return was;
}
EXPORT_SYMBOL(lkpi_kthread_mod_delayed_work);

void lkpi_kthread_flush_work(struct kthread_work *work)
{
	struct kthread_worker *w = work->worker;

	if (!w)
		return;
	wait_event(w->idle_wq, ({
		unsigned long f;
		bool busy;

		raw_spin_lock_irqsave(&w->lock, f);
		busy = !list_empty(&work->node) || w->current_work == work;
		raw_spin_unlock_irqrestore(&w->lock, f);
		!busy;
	}));
}
EXPORT_SYMBOL(lkpi_kthread_flush_work);

void lkpi_kthread_flush_worker(struct kthread_worker *w)
{
	wait_event(w->idle_wq, ({
		unsigned long f;
		bool busy;

		raw_spin_lock_irqsave(&w->lock, f);
		busy = !list_empty(&w->work_list) || w->current_work;
		raw_spin_unlock_irqrestore(&w->lock, f);
		!busy;
	}));
}
EXPORT_SYMBOL(lkpi_kthread_flush_worker);

bool lkpi_kthread_cancel_work_sync(struct kthread_work *work)
{
	bool was = lkpi_kwork_dequeue(work, false);

	lkpi_kthread_flush_work(work);
	return was;
}
EXPORT_SYMBOL(lkpi_kthread_cancel_work_sync);

bool lkpi_kthread_cancel_delayed_work_sync(struct kthread_delayed_work *dwork)
{
	bool was = lkpi_kwork_dequeue(&dwork->work, true);

	lkpi_kthread_flush_work(&dwork->work);
	return was;
}
EXPORT_SYMBOL(lkpi_kthread_cancel_delayed_work_sync);

void lkpi_kthread_destroy_worker(struct kthread_worker *w)
{
	unsigned long flags;

	if (!w)
		return;
	lkpi_kthread_flush_worker(w);
	raw_spin_lock_irqsave(&w->lock, flags);
	w->stopping = true;
	raw_spin_unlock_irqrestore(&w->lock, flags);
	wake_up(&w->wake_wq);
	kthread_stop(w->task);
	kfree(w);
}
EXPORT_SYMBOL(lkpi_kthread_destroy_worker);
