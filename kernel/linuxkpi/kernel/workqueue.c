// SPDX-License-Identifier: GPL-2.0-only
/*
 * Workqueues de Linux sobre un grupo de hilos compartido (ver
 * linux/workqueue.h para la semantica).
 *
 * Estructura (todo bajo pool.lock):
 *  - pool.worklist: trabajos listos para cualquier hilo.
 *  - wq->inactive: trabajos de una cola que ya tiene max_active en marcha.
 *  - worker->scheduled: trabajos asignados a un hilo concreto: el siguiente
 *    de un trabajo que ese hilo esta ejecutando (no reentrada) y las
 *    barreras de flush_work().
 * Un trabajo encolado tiene PENDING en data, LKPI_WORK_QUEUED y su
 * numero de secuencia de la cola; PENDING se borra justo antes de llamar a
 * la funcion, que puede liberar el work_struct: despues el hilo solo usa lo
 * que copio en su propia estructura.
 *
 * Barreras: flush_work() pone un trabajo especial justo detras del trabajo
 * (marcado LINKED, de modo que quien lo coja se lleve tambien la barrera)
 * o al principio de la lista del hilo que lo ejecuta, y espera a que la
 * barrera corra.
 *
 * El hilo lkpi_wq_mgr crea hilos cuando hay trabajo listo, ninguno libre y
 * menos ejecutandose (no dormidos) que CPUs; los que pasan un rato sin
 * trabajo terminan (y el gestor los recoge con kthread_stop).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/sprintf.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "../lkpi_internal.h"

#define LKPI_WORK_QUEUED 0x1	/* en alguna lista */
#define LKPI_WORK_ACTIVE 0x2	/* cuenta en wq->nr_active */
#define LKPI_WORK_LINKED 0x4	/* el siguiente de la lista va con este */
#define LKPI_WORK_BARRIER 0x8	/* barrera de flush_work() */

#define LKPI_WQ_MAX_WORKERS 64
#define LKPI_WQ_IDLE_TIMEOUT (10 * HZ)

struct workqueue_struct {
	char name[24];
	unsigned int flags;
	int max_active;
	int nr_active;
	unsigned long next_seq;
	struct list_head inactive;
	struct list_head node;		/* lkpi_pool.wqs */
};

struct lkpi_worker {
	struct list_head node;		/* pool.workers / pool.dead */
	struct list_head idle_node;	/* pool.idle (vacio si ocupado) */
	struct task_struct *task;
	struct list_head scheduled;
	struct work_struct *current_work;
	struct workqueue_struct *current_wq;
	unsigned long current_seq;
	bool current_active;
	bool dying;
};

struct lkpi_wq_barrier {
	struct work_struct work;
	struct completion done;
};

static struct {
	raw_spinlock_t lock;
	struct list_head worklist;
	struct list_head workers;
	struct list_head idle;
	struct list_head dead;		/* terminados, a recoger */
	struct list_head wqs;
	int nr_workers;
	int min_workers;
	bool stopping;
	struct task_struct *manager;
	struct wait_queue_head done_wq;	/* termino algun trabajo */
} lkpi_pool;

struct workqueue_struct *lkpi_system_wq;
EXPORT_SYMBOL(lkpi_system_wq);
struct workqueue_struct *lkpi_system_highpri_wq;
EXPORT_SYMBOL(lkpi_system_highpri_wq);
struct workqueue_struct *lkpi_system_long_wq;
EXPORT_SYMBOL(lkpi_system_long_wq);
struct workqueue_struct *lkpi_system_unbound_wq;
EXPORT_SYMBOL(lkpi_system_unbound_wq);
struct workqueue_struct *lkpi_system_freezable_wq;
EXPORT_SYMBOL(lkpi_system_freezable_wq);
struct workqueue_struct *lkpi_system_power_efficient_wq;
EXPORT_SYMBOL(lkpi_system_power_efficient_wq);
struct workqueue_struct *lkpi_system_freezable_power_efficient_wq;
EXPORT_SYMBOL(lkpi_system_freezable_power_efficient_wq);

static inline void lkpi_wq_lock(unsigned long *flags) { raw_spin_lock_irqsave(&lkpi_pool.lock, *flags); }
static inline void lkpi_wq_unlock(unsigned long flags) { raw_spin_unlock_irqrestore(&lkpi_pool.lock, flags); }

static inline bool lkpi_work_test_and_set_pending(struct work_struct *w)
{
	return test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w));
}
static inline void lkpi_work_clear_pending(struct work_struct *w)
{
	smp_mb__before_atomic();
	clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w));
}

/* ------------------------------------------------- listas (con lock) */

/* Mueve `w` y los que van enlazados detras al final de `dst`. */
static void lkpi_move_linked(struct work_struct *w, struct list_head *dst)
{
	for (;;) {
		struct work_struct *n = list_next_entry(w, entry);
		bool linked = w->__lkpi_flags & LKPI_WORK_LINKED;

		list_move_tail(&w->entry, dst);
		if (!linked)
			break;
		w = n;
	}
}

static struct lkpi_worker *lkpi_find_worker_executing(struct work_struct *w)
{
	struct lkpi_worker *wk;

	list_for_each_entry(wk, &lkpi_pool.workers, node)
		if (wk->current_work == w)
			return wk;
	return NULL;
}

static void lkpi_wq_kick(void)
{
	if (!list_empty(&lkpi_pool.idle)) {
		struct lkpi_worker *wk =
			list_first_entry(&lkpi_pool.idle, struct lkpi_worker, idle_node);
		/* Fuera de la lista: el siguiente aviso despierta a otro. */
		list_del_init(&wk->idle_node);
		wake_up_process(wk->task);
	} else if (lkpi_pool.manager) {
		wake_up_process(lkpi_pool.manager);
	}
}

/* Pasa de wq->inactive a la lista global lo que permita max_active. */
static void lkpi_wq_activate(struct workqueue_struct *wq)
{
	bool moved = false;

	while (!list_empty(&wq->inactive)) {
		struct work_struct *w = list_first_entry(&wq->inactive, struct work_struct, entry);

		if (!(w->__lkpi_flags & LKPI_WORK_BARRIER)) {
			if (wq->nr_active >= wq->max_active)
				break;
			wq->nr_active++;
			w->__lkpi_flags |= LKPI_WORK_ACTIVE;
		}
		lkpi_move_linked(w, &lkpi_pool.worklist);
		moved = true;
	}
	if (moved)
		lkpi_wq_kick();
}

static void __lkpi_queue_work_locked(struct workqueue_struct *wq, struct work_struct *w)
{
	w->__lkpi_wq = wq;
	w->__lkpi_seq = ++wq->next_seq;
	w->__lkpi_flags = LKPI_WORK_QUEUED;
	list_add_tail(&w->entry, &wq->inactive);
	lkpi_wq_activate(wq);
}

/* El llamante tiene PENDING. */
static void __lkpi_queue_work(struct workqueue_struct *wq, struct work_struct *w)
{
	unsigned long flags;

	if (WARN_ON_ONCE(!wq))
		wq = lkpi_system_wq;
	lkpi_wq_lock(&flags);
	__lkpi_queue_work_locked(wq, w);
	lkpi_wq_unlock(flags);
}

bool lkpi_queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	if (lkpi_work_test_and_set_pending(work))
		return false;
	__lkpi_queue_work(wq, work);
	return true;
}
EXPORT_SYMBOL(lkpi_queue_work);

/* --------------------------------------------------------------- hilos */

/* Con lock. Saca el siguiente trabajo para `me` (o NULL). */
static struct work_struct *lkpi_wq_pick(struct lkpi_worker *me)
{
	while (list_empty(&me->scheduled) && !list_empty(&lkpi_pool.worklist)) {
		struct work_struct *w =
			list_first_entry(&lkpi_pool.worklist, struct work_struct, entry);
		struct lkpi_worker *other = NULL;

		if (!(w->__lkpi_flags & LKPI_WORK_BARRIER))
			other = lkpi_find_worker_executing(w);
		/* No reentrada: lo ejecuta, cuando acabe, el hilo que ya lo tiene. */
		lkpi_move_linked(w, other ? &other->scheduled : &me->scheduled);
	}
	if (list_empty(&me->scheduled))
		return NULL;
	return list_first_entry(&me->scheduled, struct work_struct, entry);
}

/* Con lock; lo suelta mientras corre la funcion. */
static void lkpi_wq_process_one(struct lkpi_worker *me, struct work_struct *w,
				unsigned long *flags)
{
	struct workqueue_struct *wq = w->__lkpi_wq;
	work_func_t fn = w->func;

	list_del_init(&w->entry);
	if (w->__lkpi_flags & LKPI_WORK_BARRIER) {
		struct lkpi_wq_barrier *b = container_of(w, struct lkpi_wq_barrier, work);

		w->__lkpi_flags = 0;
		lkpi_wq_unlock(*flags);
		complete(&b->done);
		lkpi_wq_lock(flags);
		return;
	}
	me->current_work = w;
	me->current_wq = wq;
	me->current_seq = w->__lkpi_seq;
	me->current_active = w->__lkpi_flags & LKPI_WORK_ACTIVE;
	w->__lkpi_flags = 0;
	/* Desde aqui se puede volver a encolar (y la funcion puede liberarlo). */
	lkpi_work_clear_pending(w);
	lkpi_wq_unlock(*flags);

	fn(w);
	WARN_ONCE(lkpi_preempt_count(), "workqueue: %ps volvio con la expulsion desactivada\n", fn);

	lkpi_wq_lock(flags);
	me->current_work = NULL;
	if (me->current_active) {
		wq->nr_active--;
		lkpi_wq_activate(wq);
	}
	me->current_wq = NULL;
	if (waitqueue_active(&lkpi_pool.done_wq))
		wake_up_all(&lkpi_pool.done_wq);
}

static int lkpi_worker_fn(void *data)
{
	struct lkpi_worker *me = data;
	unsigned long flags;

	current->flags |= PF_WQ_WORKER;
	lkpi_wq_lock(&flags);
	for (;;) {
		struct work_struct *w = lkpi_wq_pick(me);
		long left;

		if (w) {
			lkpi_wq_process_one(me, w, &flags);
			continue;
		}
		if (me->dying || lkpi_pool.stopping)
			break;
		list_add(&me->idle_node, &lkpi_pool.idle);
		set_current_state(TASK_INTERRUPTIBLE);
		lkpi_wq_unlock(flags);
		left = schedule_timeout(LKPI_WQ_IDLE_TIMEOUT);
		lkpi_wq_lock(&flags);
		list_del_init(&me->idle_node);
		/* Mucho rato sin trabajo y sobran hilos: este termina. */
		if (!left && lkpi_pool.nr_workers > lkpi_pool.min_workers &&
		    list_empty(&lkpi_pool.worklist) && list_empty(&me->scheduled)) {
			me->dying = true;
			lkpi_pool.nr_workers--;
			list_move(&me->node, &lkpi_pool.dead);
			if (lkpi_pool.manager)
				wake_up_process(lkpi_pool.manager);
			break;
		}
	}
	lkpi_wq_unlock(flags);
	/* Espera a kthread_stop() (gestor o descarga). */
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int lkpi_wq_spawn(void)
{
	struct lkpi_worker *wk = kzalloc(sizeof(*wk), GFP_KERNEL);
	struct task_struct *p;
	unsigned long flags;
	static atomic_t id = ATOMIC_INIT(0);

	if (!wk)
		return -ENOMEM;
	INIT_LIST_HEAD(&wk->scheduled);
	INIT_LIST_HEAD(&wk->idle_node);
	p = lkpi_kthread_create(lkpi_worker_fn, wk, "lkpi_wq/%d", atomic_inc_return(&id));
	if (IS_ERR(p)) {
		kfree(wk);
		return PTR_ERR(p);
	}
	wk->task = p;
	lkpi_wq_lock(&flags);
	list_add_tail(&wk->node, &lkpi_pool.workers);
	lkpi_pool.nr_workers++;
	lkpi_wq_unlock(flags);
	wake_up_process(p);
	return 0;
}

/* Con lock: hilos con trabajo que no estan dormidos. */
static int lkpi_wq_nr_running(void)
{
	struct lkpi_worker *wk;
	int n = 0;

	list_for_each_entry(wk, &lkpi_pool.workers, node)
		if (wk->current_work && READ_ONCE(wk->task->__state) == TASK_RUNNING)
			n++;
	return n;
}

static int lkpi_wq_manager_fn(void *unused)
{
	while (!kthread_should_stop()) {
		struct lkpi_worker *dead = NULL;
		unsigned long flags;
		bool need, busy;

		lkpi_wq_lock(&flags);
		if (!list_empty(&lkpi_pool.dead)) {
			dead = list_first_entry(&lkpi_pool.dead, struct lkpi_worker, node);
			list_del(&dead->node);
		}
		busy = !list_empty(&lkpi_pool.worklist);
		need = busy && list_empty(&lkpi_pool.idle) && !lkpi_pool.stopping &&
		       lkpi_pool.nr_workers < LKPI_WQ_MAX_WORKERS &&
		       lkpi_wq_nr_running() < (int)num_online_cpus();
		set_current_state(TASK_INTERRUPTIBLE);
		lkpi_wq_unlock(flags);

		if (dead) {
			__set_current_state(TASK_RUNNING);
			kthread_stop(dead->task);
			kfree(dead);
			continue;
		}
		if (need) {
			__set_current_state(TASK_RUNNING);
			if (lkpi_wq_spawn())
				schedule_timeout_uninterruptible(1);
			continue;
		}
		/* Con trabajo pendiente, revisa a menudo por si los hilos se duermen. */
		schedule_timeout(busy ? 1 : MAX_SCHEDULE_TIMEOUT);
	}
	return 0;
}

struct work_struct *lkpi_current_work(void)
{
	struct task_struct *p = current;
	struct lkpi_worker *wk;

	if (!(p->flags & PF_WQ_WORKER))
		return NULL;
	wk = kthread_data(p);
	return wk ? wk->current_work : NULL;
}
EXPORT_SYMBOL(lkpi_current_work);

/* ----------------------------------------------------------- flush */

/*
 * Pone una barrera detras de la ultima instancia de `w` (encolada o en
 * curso). Devuelve false si no hay ninguna.
 */
static bool lkpi_start_flush_work(struct work_struct *w, struct lkpi_wq_barrier *b)
{
	struct lkpi_worker *wk;
	unsigned long flags;

	__lkpi_init_work(&b->work, NULL);
	b->work.__lkpi_flags = LKPI_WORK_BARRIER | LKPI_WORK_QUEUED;
	init_completion(&b->done);

	lkpi_wq_lock(&flags);
	if (w->__lkpi_flags & LKPI_WORK_QUEUED) {
		b->work.__lkpi_flags |= w->__lkpi_flags & LKPI_WORK_LINKED;
		w->__lkpi_flags |= LKPI_WORK_LINKED;
		list_add(&b->work.entry, &w->entry);
	} else if ((wk = lkpi_find_worker_executing(w))) {
		list_add(&b->work.entry, &wk->scheduled);
	} else {
		lkpi_wq_unlock(flags);
		return false;
	}
	lkpi_wq_unlock(flags);
	return true;
}

bool lkpi_flush_work(struct work_struct *work)
{
	struct lkpi_wq_barrier b;

	if (!lkpi_start_flush_work(work, &b))
		return false;
	wait_for_completion(&b.done);
	return true;
}
EXPORT_SYMBOL(lkpi_flush_work);

/* Con lock: menor secuencia de `wq` aun en vuelo (encolada o en curso). */
static unsigned long lkpi_wq_min_inflight(struct workqueue_struct *wq)
{
	unsigned long min = ULONG_MAX;
	struct lkpi_worker *wk;
	struct work_struct *w;

#define __LKPI_SCAN(head) \
	list_for_each_entry(w, head, entry) \
		if (w->__lkpi_wq == wq && !(w->__lkpi_flags & LKPI_WORK_BARRIER) && \
		    w->__lkpi_seq < min) \
			min = w->__lkpi_seq
	__LKPI_SCAN(&lkpi_pool.worklist);
	__LKPI_SCAN(&wq->inactive);
	list_for_each_entry(wk, &lkpi_pool.workers, node) {
		__LKPI_SCAN(&wk->scheduled);
		if (wk->current_work && wk->current_wq == wq && wk->current_seq < min)
			min = wk->current_seq;
	}
#undef __LKPI_SCAN
	return min;
}

static bool lkpi_wq_flushed(struct workqueue_struct *wq, unsigned long upto)
{
	unsigned long flags;
	bool done;

	lkpi_wq_lock(&flags);
	done = lkpi_wq_min_inflight(wq) > upto;
	lkpi_wq_unlock(flags);
	return done;
}

void lkpi_flush_workqueue(struct workqueue_struct *wq)
{
	unsigned long flags, upto;

	lkpi_wq_lock(&flags);
	upto = wq->next_seq;
	lkpi_wq_unlock(flags);
	wait_event(lkpi_pool.done_wq, lkpi_wq_flushed(wq, upto));
}
EXPORT_SYMBOL(lkpi_flush_workqueue);

/* Repite hasta que no quede nada (los trabajos pueden encolar otros). */
void lkpi_drain_workqueue(struct workqueue_struct *wq)
{
	for (;;) {
		lkpi_flush_workqueue(wq);
		if (lkpi_wq_flushed(wq, ULONG_MAX - 1))
			break;
	}
}
EXPORT_SYMBOL(lkpi_drain_workqueue);

/* ----------------------------------------------------------- delayed */

void lkpi_timer_delayed_work_fn(struct timer_list *t)
{
	struct delayed_work *dw = from_timer(dw, t, timer);
	__lkpi_queue_work(dw->wq, &dw->work);
}
EXPORT_SYMBOL(lkpi_timer_delayed_work_fn);

/* El llamante tiene PENDING. */
static void __lkpi_queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
				      unsigned long delay)
{
	if (!delay) {
		__lkpi_queue_work(wq, &dw->work);
		return;
	}
	dw->wq = wq;
	mod_timer(&dw->timer, jiffies + delay);
}

bool lkpi_queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
			     unsigned long delay)
{
	if (lkpi_work_test_and_set_pending(&dw->work))
		return false;
	__lkpi_queue_delayed_work(wq, dw, delay);
	return true;
}
EXPORT_SYMBOL(lkpi_queue_delayed_work);

/*
 * Se queda con PENDING. 1: estaba pendiente (y se ha quitado de su lista o
 * su timer); 0: no lo estaba; -EAGAIN: transitorio (el timer acaba de
 * vencer y se esta encolando), reintentar.
 */
static int lkpi_try_to_grab_pending(struct work_struct *w, bool is_dwork)
{
	unsigned long flags;

	if (is_dwork && timer_delete(&to_delayed_work(w)->timer))
		return 1;
	if (!lkpi_work_test_and_set_pending(w))
		return 0;
	lkpi_wq_lock(&flags);
	if (w->__lkpi_flags & LKPI_WORK_QUEUED) {
		struct workqueue_struct *wq = w->__lkpi_wq;
		bool active = w->__lkpi_flags & LKPI_WORK_ACTIVE;

		/* Una barrera enlazada detras se queda: correra sola. */
		list_del_init(&w->entry);
		w->__lkpi_flags = 0;
		if (active) {
			wq->nr_active--;
			lkpi_wq_activate(wq);
		}
		lkpi_wq_unlock(flags);
		return 1;
	}
	lkpi_wq_unlock(flags);
	return -EAGAIN;
}

static int lkpi_grab_pending(struct work_struct *w, bool is_dwork)
{
	int ret;

	while ((ret = lkpi_try_to_grab_pending(w, is_dwork)) == -EAGAIN)
		yield();
	return ret;
}

bool lkpi_mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
			   unsigned long delay)
{
	int ret = lkpi_grab_pending(&dw->work, true);

	__lkpi_queue_delayed_work(wq, dw, delay);
	return ret == 1;
}
EXPORT_SYMBOL(lkpi_mod_delayed_work);

static bool __lkpi_cancel(struct work_struct *w, bool is_dwork, bool sync)
{
	int ret = lkpi_grab_pending(w, is_dwork);

	/* Con PENDING en nuestras manos nadie lo vuelve a encolar. */
	if (sync)
		lkpi_flush_work(w);
	lkpi_work_clear_pending(w);
	return ret == 1;
}

bool lkpi_cancel_work(struct work_struct *work)
{
	return __lkpi_cancel(work, false, false);
}
EXPORT_SYMBOL(lkpi_cancel_work);

bool lkpi_cancel_work_sync(struct work_struct *work)
{
	return __lkpi_cancel(work, false, true);
}
EXPORT_SYMBOL(lkpi_cancel_work_sync);

bool lkpi_cancel_delayed_work(struct delayed_work *dw)
{
	return __lkpi_cancel(&dw->work, true, false);
}
EXPORT_SYMBOL(lkpi_cancel_delayed_work);

bool lkpi_cancel_delayed_work_sync(struct delayed_work *dw)
{
	return __lkpi_cancel(&dw->work, true, true);
}
EXPORT_SYMBOL(lkpi_cancel_delayed_work_sync);

bool lkpi_flush_delayed_work(struct delayed_work *dw)
{
	if (timer_delete_sync(&dw->timer))
		__lkpi_queue_work(dw->wq, &dw->work);
	return lkpi_flush_work(&dw->work);
}
EXPORT_SYMBOL(lkpi_flush_delayed_work);

static void lkpi_rcu_work_rcufn(struct rcu_head *rcu)
{
	struct rcu_work *rw = container_of(rcu, struct rcu_work, rcu);
	__lkpi_queue_work(rw->wq, &rw->work);
}

bool lkpi_queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rw)
{
	if (lkpi_work_test_and_set_pending(&rw->work))
		return false;
	rw->wq = wq;
	call_rcu(&rw->rcu, lkpi_rcu_work_rcufn);
	return true;
}
EXPORT_SYMBOL(lkpi_queue_rcu_work);

bool lkpi_flush_rcu_work(struct rcu_work *rw)
{
	if (work_pending(&rw->work)) {
		rcu_barrier();
		lkpi_flush_work(&rw->work);
		return true;
	}
	return lkpi_flush_work(&rw->work);
}
EXPORT_SYMBOL(lkpi_flush_rcu_work);

/* ------------------------------------------------------------ colas */

struct workqueue_struct *lkpi_alloc_workqueue(const char *fmt, unsigned int flags,
					      int max_active, ...)
{
	struct workqueue_struct *wq = kzalloc(sizeof(*wq), GFP_KERNEL);
	unsigned long irqf;
	va_list ap;

	if (!wq)
		return NULL;
	va_start(ap, max_active);
	vsnprintf(wq->name, sizeof(wq->name), fmt, ap);
	va_end(ap);
	if (flags & __WQ_ORDERED)
		max_active = 1;
	if (max_active <= 0)
		max_active = WQ_DFL_ACTIVE;
	wq->flags = flags;
	wq->max_active = min_t(int, max_active, WQ_MAX_ACTIVE);
	INIT_LIST_HEAD(&wq->inactive);
	lkpi_wq_lock(&irqf);
	list_add_tail(&wq->node, &lkpi_pool.wqs);
	lkpi_wq_unlock(irqf);
	return wq;
}
EXPORT_SYMBOL(lkpi_alloc_workqueue);

void lkpi_destroy_workqueue(struct workqueue_struct *wq)
{
	unsigned long flags;

	if (!wq)
		return;
	lkpi_drain_workqueue(wq);
	lkpi_wq_lock(&flags);
	list_del(&wq->node);
	lkpi_wq_unlock(flags);
	kfree(wq);
}
EXPORT_SYMBOL(lkpi_destroy_workqueue);

void lkpi_workqueue_set_max_active(struct workqueue_struct *wq, int max_active)
{
	unsigned long flags;

	if (wq->flags & __WQ_ORDERED)
		return;
	lkpi_wq_lock(&flags);
	wq->max_active = clamp_t(int, max_active, 1, WQ_MAX_ACTIVE);
	lkpi_wq_activate(wq);
	lkpi_wq_unlock(flags);
}
EXPORT_SYMBOL(lkpi_workqueue_set_max_active);

bool lkpi_workqueue_congested(int cpu, struct workqueue_struct *wq)
{
	(void)cpu;
	return !list_empty(&wq->inactive);
}
EXPORT_SYMBOL(lkpi_workqueue_congested);

/* ------------------------------------------------------- init / exit */

int lkpi_wq_init(void)
{
	int i, ret;

	raw_spin_lock_init(&lkpi_pool.lock);
	INIT_LIST_HEAD(&lkpi_pool.worklist);
	INIT_LIST_HEAD(&lkpi_pool.workers);
	INIT_LIST_HEAD(&lkpi_pool.idle);
	INIT_LIST_HEAD(&lkpi_pool.dead);
	INIT_LIST_HEAD(&lkpi_pool.wqs);
	init_waitqueue_head(&lkpi_pool.done_wq);
	lkpi_pool.stopping = false;
	lkpi_pool.min_workers = max_t(int, 2, num_online_cpus());
	for (i = 0; i < lkpi_pool.min_workers; i++) {
		ret = lkpi_wq_spawn();
		if (ret)
			goto fail;
	}
	lkpi_pool.manager = lkpi_kthread_create_internal(lkpi_wq_manager_fn, NULL, "lkpi_wq_mgr");
	if (IS_ERR(lkpi_pool.manager)) {
		ret = PTR_ERR(lkpi_pool.manager);
		lkpi_pool.manager = NULL;
		goto fail;
	}
	lkpi_system_wq = alloc_workqueue("events", 0, 0);
	lkpi_system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 0);
	lkpi_system_long_wq = alloc_workqueue("events_long", 0, 0);
	lkpi_system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND, WQ_MAX_ACTIVE);
	lkpi_system_freezable_wq = alloc_workqueue("events_freezable", WQ_FREEZABLE, 0);
	lkpi_system_power_efficient_wq =
		alloc_workqueue("events_power_efficient", WQ_POWER_EFFICIENT, 0);
	lkpi_system_freezable_power_efficient_wq =
		alloc_workqueue("events_freezable_pwr_ef", WQ_FREEZABLE | WQ_POWER_EFFICIENT, 0);
	if (!lkpi_system_wq || !lkpi_system_highpri_wq || !lkpi_system_long_wq ||
	    !lkpi_system_unbound_wq || !lkpi_system_freezable_wq ||
	    !lkpi_system_power_efficient_wq || !lkpi_system_freezable_power_efficient_wq) {
		ret = -ENOMEM;
		goto fail;
	}
	return 0;
fail:
	lkpi_wq_exit();
	return ret;
}

void lkpi_wq_exit(void)
{
	struct workqueue_struct **sys[] = {
		&lkpi_system_wq, &lkpi_system_highpri_wq, &lkpi_system_long_wq,
		&lkpi_system_unbound_wq, &lkpi_system_freezable_wq,
		&lkpi_system_power_efficient_wq, &lkpi_system_freezable_power_efficient_wq,
	};
	struct lkpi_worker *wk;
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sys); i++) {
		destroy_workqueue(*sys[i]);
		*sys[i] = NULL;
	}
	WARN(!list_empty(&lkpi_pool.wqs), "linuxkpi: workqueues sin destruir al descargar\n");
	if (lkpi_pool.manager) {
		kthread_stop(lkpi_pool.manager);
		lkpi_pool.manager = NULL;
	}
	lkpi_wq_lock(&flags);
	lkpi_pool.stopping = true;
	list_splice_init(&lkpi_pool.workers, &lkpi_pool.dead);
	lkpi_wq_unlock(flags);
	while (!list_empty(&lkpi_pool.dead)) {
		wk = list_first_entry(&lkpi_pool.dead, struct lkpi_worker, node);
		list_del(&wk->node);
		wake_up_process(wk->task);
		kthread_stop(wk->task);
		kfree(wk);
	}
	lkpi_pool.nr_workers = 0;
}
