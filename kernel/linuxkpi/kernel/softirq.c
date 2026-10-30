// SPDX-License-Identifier: GPL-2.0-only
/*
 * "Softirq" de la capa: un hilo (lkpi_softirqd) que ejecuta irq_work,
 * tasklets, timer_list y hrtimer, cada funcion con la expulsion desactivada
 * como en Linux. Correr en un hilo, y no en la IRQ del tick, permite que
 * spin_lock_bh() sea un spin_lock() normal.
 *
 * Los timers estan en listas ordenadas por vencimiento. El hilo duerme hasta
 * el primero (resolucion de los plazos de Aurora, 1 ms) o hasta que llega
 * trabajo nuevo. Quien espera a que termine una funcion en curso
 * (del_timer_sync, hrtimer_cancel, tasklet_kill, irq_work_sync) duerme en
 * lkpi_sq_done.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/llist.h>
#include <linux/math64.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>

#include "../lkpi_internal.h"

static DEFINE_RAW_SPINLOCK(lkpi_sq_lock);
static LIST_HEAD(lkpi_timers);			/* por expires (jiffies) */
static LIST_HEAD(lkpi_hrtimers);		/* por node.expires (ns) */
static struct tasklet_struct *lkpi_tl_head;
static struct tasklet_struct **lkpi_tl_tail = &lkpi_tl_head;
static LLIST_HEAD(lkpi_irq_works);
static struct timer_list *lkpi_running_timer;
static struct hrtimer *lkpi_running_hrtimer;
static DECLARE_WAIT_QUEUE_HEAD(lkpi_sq_done);
static struct task_struct *lkpi_softirqd;
static int lkpi_sq_kick;

static void lkpi_sq_wake(void)
{
	WRITE_ONCE(lkpi_sq_kick, 1);
	smp_mb();
	if (lkpi_softirqd)
		wake_up_process(lkpi_softirqd);
}

static void lkpi_sq_done_wake(void)
{
	if (wq_has_sleeper(&lkpi_sq_done))
		wake_up_all(&lkpi_sq_done);
}

/* ---------------------------------------------------------- timer_list */

static void lkpi_timer_detach(struct timer_list *t)
{
	list_del(&t->entry);
	WRITE_ONCE(t->entry.next, NULL);
}

int lkpi_mod_timer(struct timer_list *t, unsigned long expires, unsigned int mode)
{
	struct timer_list *pos;
	unsigned long flags;
	bool first;
	int pending;

	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	pending = timer_pending(t);
	if ((mode == __LKPI_MOD_PENDING_ONLY && !pending) ||
	    (mode == __LKPI_MOD_REDUCE && pending && time_before_eq(t->expires, expires)) ||
	    !t->function /* timer_shutdown() */) {
		raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
		return pending;
	}
	if (pending)
		lkpi_timer_detach(t);
	t->expires = expires;
	list_for_each_entry(pos, &lkpi_timers, entry)
		if (time_before(expires, pos->expires))
			break;
	list_add_tail(&t->entry, &pos->entry);
	first = lkpi_timers.next == &t->entry;
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	if (first)
		lkpi_sq_wake();
	return pending;
}
EXPORT_SYMBOL(lkpi_mod_timer);

int lkpi_timer_delete(struct timer_list *t, bool shutdown)
{
	unsigned long flags;
	int pending;

	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	pending = timer_pending(t);
	if (pending)
		lkpi_timer_detach(t);
	if (shutdown)
		t->function = NULL;
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	return pending;
}
EXPORT_SYMBOL(lkpi_timer_delete);

int lkpi_try_to_del_timer_sync(struct timer_list *t)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	if (lkpi_running_timer == t) {
		ret = -1;
	} else {
		ret = timer_pending(t);
		if (ret)
			lkpi_timer_detach(t);
	}
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	return ret;
}
EXPORT_SYMBOL(lkpi_try_to_del_timer_sync);

int lkpi_timer_delete_sync(struct timer_list *t, bool shutdown)
{
	unsigned long flags;
	bool running;
	int ret = 0;

	WARN_ON_ONCE(current == lkpi_softirqd && lkpi_running_timer == t);
	for (;;) {
		raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
		if (timer_pending(t)) {
			lkpi_timer_detach(t);
			ret = 1;
		}
		if (shutdown)
			t->function = NULL;
		running = lkpi_running_timer == t;
		raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
		if (!running)
			return ret;
		/* La funcion pudo volver a armarlo: otra vuelta tras esperar. */
		wait_event(lkpi_sq_done, READ_ONCE(lkpi_running_timer) != t);
	}
}
EXPORT_SYMBOL(lkpi_timer_delete_sync);

/* Ejecuta los vencidos; devuelve los jiffies hasta el siguiente (o MAX). */
static long lkpi_run_timers(void)
{
	unsigned long flags;
	long next = MAX_SCHEDULE_TIMEOUT;

	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	while (!list_empty(&lkpi_timers)) {
		struct timer_list *t = list_first_entry(&lkpi_timers, struct timer_list, entry);
		void (*fn)(struct timer_list *) = t->function;
		unsigned long now = jiffies;

		if (time_before(now, t->expires)) {
			next = (long)(t->expires - now);
			break;
		}
		lkpi_timer_detach(t);
		lkpi_running_timer = t;
		raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);

		preempt_disable();
		fn(t);
		preempt_enable();

		raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
		lkpi_running_timer = NULL;
		raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
		lkpi_sq_done_wake();
		raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	}
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	return next;
}

/* ------------------------------------------------------------- hrtimer */

void lkpi_hrtimer_setup(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *),
			clockid_t clock_id, enum hrtimer_mode mode)
{
	t->node.link.next = NULL;
	t->node.link.prev = NULL;
	t->_softexpires = t->node.expires = 0;
	t->function = fn;
	t->clockid = clock_id;
	t->state = HRTIMER_STATE_INACTIVE;
	t->is_rel = mode & HRTIMER_MODE_REL;
}
EXPORT_SYMBOL(lkpi_hrtimer_setup);

/* Con lkpi_sq_lock. Devuelve true si queda el primero. */
static bool lkpi_hrtimer_enqueue(struct hrtimer *t)
{
	struct hrtimer *pos;

	list_for_each_entry(pos, &lkpi_hrtimers, node.link)
		if (t->node.expires < pos->node.expires)
			break;
	list_add_tail(&t->node.link, &pos->node.link);
	t->state = HRTIMER_STATE_ENQUEUED;
	return lkpi_hrtimers.next == &t->node.link;
}

static void lkpi_hrtimer_dequeue(struct hrtimer *t)
{
	list_del(&t->node.link);
	t->node.link.next = NULL;
	t->state = HRTIMER_STATE_INACTIVE;
}

void lkpi_hrtimer_start_range_ns(struct hrtimer *t, ktime_t tim, u64 range_ns,
				 enum hrtimer_mode mode)
{
	unsigned long flags;
	bool first;

	if (mode & HRTIMER_MODE_REL)
		tim = ktime_add(ktime_get(), tim);
	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	if (t->state & HRTIMER_STATE_ENQUEUED)
		lkpi_hrtimer_dequeue(t);
	hrtimer_set_expires_range_ns(t, tim, range_ns);
	first = lkpi_hrtimer_enqueue(t);
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	if (first)
		lkpi_sq_wake();
}
EXPORT_SYMBOL(lkpi_hrtimer_start_range_ns);

int lkpi_hrtimer_try_to_cancel(struct hrtimer *t)
{
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	if (t->state & HRTIMER_STATE_ENQUEUED) {
		lkpi_hrtimer_dequeue(t);
		ret = 1;
	} else if (lkpi_running_hrtimer == t) {
		ret = -1;
	}
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	return ret;
}
EXPORT_SYMBOL(lkpi_hrtimer_try_to_cancel);

int lkpi_hrtimer_cancel(struct hrtimer *t)
{
	int ret;

	for (;;) {
		ret = lkpi_hrtimer_try_to_cancel(t);
		if (ret >= 0)
			return ret;
		wait_event(lkpi_sq_done, READ_ONCE(lkpi_running_hrtimer) != t);
	}
}
EXPORT_SYMBOL(lkpi_hrtimer_cancel);

bool lkpi_hrtimer_active(const struct hrtimer *t)
{
	return (READ_ONCE(t->state) & HRTIMER_STATE_ENQUEUED) ||
	       READ_ONCE(lkpi_running_hrtimer) == t;
}
EXPORT_SYMBOL(lkpi_hrtimer_active);

/* Avanza el vencimiento en multiplos de `interval` hasta pasar `now`. */
u64 lkpi_hrtimer_forward(struct hrtimer *t, ktime_t now, ktime_t interval)
{
	ktime_t delta = now - t->node.expires;
	u64 orun = 1;

	if (delta < 0)
		return 0;
	if (interval < 1000)
		interval = 1000;	/* como la resolucion minima de Linux */
	if (delta >= interval) {
		orun = div64_u64(delta, interval);
		hrtimer_add_expires_ns(t, orun * interval);
		if (t->node.expires > now)
			return orun;
		orun++;
	}
	hrtimer_add_expires_ns(t, interval);
	return orun;
}
EXPORT_SYMBOL(lkpi_hrtimer_forward);

/* Ejecuta los vencidos; devuelve los jiffies hasta el siguiente (o MAX). */
static long lkpi_run_hrtimers(void)
{
	unsigned long flags;
	long next = MAX_SCHEDULE_TIMEOUT;

	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	while (!list_empty(&lkpi_hrtimers)) {
		struct hrtimer *t = list_first_entry(&lkpi_hrtimers, struct hrtimer, node.link);
		enum hrtimer_restart (*fn)(struct hrtimer *) = t->function;
		enum hrtimer_restart r = HRTIMER_NORESTART;
		ktime_t now = ktime_get();

		if (now < t->node.expires) {
			next = (long)nsecs_to_jiffies(t->node.expires - now);
			break;
		}
		lkpi_hrtimer_dequeue(t);
		lkpi_running_hrtimer = t;
		raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);

		if (fn) {
			preempt_disable();
			r = fn(t);
			preempt_enable();
		}

		raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
		/* Si la funcion no lo rearmo con hrtimer_start(), lo hace RESTART. */
		if (r == HRTIMER_RESTART && !(t->state & HRTIMER_STATE_ENQUEUED))
			lkpi_hrtimer_enqueue(t);
		lkpi_running_hrtimer = NULL;
		raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
		lkpi_sq_done_wake();
		raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	}
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	return next;
}

/* ------------------------------------------------------------ tasklets */

void lkpi_tasklet_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	if (test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		return;
	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	t->next = NULL;
	*lkpi_tl_tail = t;
	lkpi_tl_tail = &t->next;
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	lkpi_sq_wake();
}
EXPORT_SYMBOL(lkpi_tasklet_schedule);

void lkpi_tasklet_enable(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	atomic_dec(&t->count);
	/* Pudo quedar aplazado mientras estaba desactivado. */
	if (test_bit(TASKLET_STATE_SCHED, &t->state))
		lkpi_sq_wake();
}
EXPORT_SYMBOL(lkpi_tasklet_enable);

void lkpi_tasklet_unlock_wait(struct tasklet_struct *t)
{
	wait_event(lkpi_sq_done, !test_bit(TASKLET_STATE_RUN, &t->state));
}
EXPORT_SYMBOL(lkpi_tasklet_unlock_wait);

void lkpi_tasklet_kill(struct tasklet_struct *t)
{
	/* Si esta encolado, deja que corra (como Linux) y espera a que acabe. */
	wait_event(lkpi_sq_done, !test_bit(TASKLET_STATE_SCHED, &t->state));
	lkpi_tasklet_unlock_wait(t);
}
EXPORT_SYMBOL(lkpi_tasklet_kill);

/* Devuelve true si quedaron tasklets desactivados aplazados. */
static bool lkpi_run_tasklets(void)
{
	struct tasklet_struct *list, *t;
	unsigned long flags;
	bool deferred = false;

	raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
	list = lkpi_tl_head;
	lkpi_tl_head = NULL;
	lkpi_tl_tail = &lkpi_tl_head;
	raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);

	while ((t = list)) {
		list = t->next;
		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				clear_bit(TASKLET_STATE_SCHED, &t->state);
				preempt_disable();
				if (t->use_callback)
					t->callback(t);
				else
					t->func(t->data);
				preempt_enable();
				tasklet_unlock(t);
				lkpi_sq_done_wake();
				continue;
			}
			tasklet_unlock(t);
		}
		/* Desactivado (o en curso): vuelve a la cola, sigue SCHED. */
		deferred = true;
		raw_spin_lock_irqsave(&lkpi_sq_lock, flags);
		t->next = NULL;
		*lkpi_tl_tail = t;
		lkpi_tl_tail = &t->next;
		raw_spin_unlock_irqrestore(&lkpi_sq_lock, flags);
	}
	return deferred;
}

/* ------------------------------------------------------------ irq_work */

bool lkpi_irq_work_queue(struct irq_work *w)
{
	int old = atomic_fetch_or(IRQ_WORK_CLAIMED, &w->flags);

	if (old & IRQ_WORK_PENDING)
		return false;
	llist_add(&w->llnode, &lkpi_irq_works);
	lkpi_sq_wake();
	return true;
}
EXPORT_SYMBOL(lkpi_irq_work_queue);

void lkpi_irq_work_sync(struct irq_work *w)
{
	wait_event(lkpi_sq_done, !irq_work_is_busy(w));
}
EXPORT_SYMBOL(lkpi_irq_work_sync);

static void lkpi_run_irq_works(void)
{
	struct llist_node *list = llist_reverse_order(llist_del_all(&lkpi_irq_works));
	struct irq_work *w, *n;

	llist_for_each_entry_safe(w, n, list, llnode) {
		/* Sin PENDING antes de llamar: se puede volver a encolar desde func. */
		int flags = atomic_fetch_andnot(IRQ_WORK_PENDING, &w->flags) & ~IRQ_WORK_PENDING;

		preempt_disable();
		w->func(w);
		preempt_enable();
		/* BUSY fuera, salvo que se haya vuelto a encolar entretanto. */
		atomic_cmpxchg(&w->flags, flags, flags & ~IRQ_WORK_BUSY);
	}
	lkpi_sq_done_wake();
}

/* ---------------------------------------------------------------- hilo */

static int lkpi_softirqd_fn(void *unused)
{
	current->__lkpi_softirq = 1;
	while (!kthread_should_stop()) {
		long next_t, next_h, timeout;
		bool deferred;

		WRITE_ONCE(lkpi_sq_kick, 0);
		smp_mb();
		lkpi_run_irq_works();
		deferred = lkpi_run_tasklets();
		next_t = lkpi_run_timers();
		next_h = lkpi_run_hrtimers();
		timeout = min(next_t, next_h);
		if (deferred && timeout > 1)
			timeout = 1;	/* tasklets desactivados: reintenta pronto */

		set_current_state(TASK_INTERRUPTIBLE);
		if (READ_ONCE(lkpi_sq_kick) || kthread_should_stop() || timeout <= 0) {
			__set_current_state(TASK_RUNNING);
			continue;
		}
		schedule_timeout(timeout);
	}
	current->__lkpi_softirq = 0;
	return 0;
}

int lkpi_softirq_init(void)
{
	struct task_struct *p = lkpi_kthread_create_internal(lkpi_softirqd_fn, NULL,
							     "lkpi_softirqd");

	if (IS_ERR(p))
		return PTR_ERR(p);
	smp_store_release(&lkpi_softirqd, p);
	return 0;
}

void lkpi_softirq_exit(void)
{
	struct task_struct *p = lkpi_softirqd;

	WARN(!list_empty(&lkpi_timers) || !list_empty(&lkpi_hrtimers) || lkpi_tl_head,
	     "linuxkpi: quedan timers o tasklets al descargar\n");
	lkpi_softirqd = NULL;
	kthread_stop(p);
}
