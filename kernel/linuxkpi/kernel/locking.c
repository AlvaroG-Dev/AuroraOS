// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cerrojos que duermen: mutex (con ww_mutex encima), rw_semaphore y
 * semaphore. Todos usan esperas FIFO con concesion directa: quien suelta
 * entrega el cerrojo al primero de la cola (marca `granted`) antes de
 * despertarlo, asi que no hay inanicion ni despertares perdidos.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/ww_mutex.h>

#include "../lkpi_internal.h"

/* ---------------------------------------------------------------- mutex */

#define LKPI_MUTEX_WAITERS 0x1UL
#define LKPI_MUTEX_FLAGS 0x7UL

struct lkpi_mutex_waiter {
	struct list_head list;
	struct task_struct *task;
	bool granted;
};

static inline struct task_struct *lkpi_owner_task(long owner)
{
	return (struct task_struct *)(owner & ~LKPI_MUTEX_FLAGS);
}

struct task_struct *lkpi_mutex_owner(struct mutex *m)
{
	return lkpi_owner_task(atomic_long_read(&m->owner));
}
EXPORT_SYMBOL(lkpi_mutex_owner);

int lkpi_mutex_trylock(struct mutex *m)
{
	long zero = 0;
	return atomic_long_try_cmpxchg(&m->owner, &zero, (long)current);
}
EXPORT_SYMBOL(lkpi_mutex_trylock);

static int __lkpi_ww_check(struct ww_mutex *ww, struct ww_acquire_ctx *ctx);
static void __lkpi_ww_wound(struct ww_mutex *ww, struct ww_acquire_ctx *ctx);
static void __lkpi_ww_acquired_locked(struct ww_mutex *ww, struct ww_acquire_ctx *ctx);

/*
 * Camino lento comun. Con `ww`, ademas de esperar, comprueba en cada vuelta
 * si el contexto debe morir (-EDEADLK) y, en wound-wait, hiere al dueno.
 * Con `ww` y exito, publica el contexto bajo wait_lock antes de soltarlo.
 */
static int __lkpi_mutex_lock_slow(struct mutex *m, unsigned int state, struct ww_mutex *ww,
				  struct ww_acquire_ctx *ctx, bool may_die)
{
	struct task_struct *me = current;
	struct lkpi_mutex_waiter w = { .task = me, .granted = false };
	bool queued = false;
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&m->wait_lock, flags);
	for (;;) {
		long o;

		/*
		 * El estado va antes de mirar las condiciones: wound-wait marca
		 * `wounded` y despierta desde el wait_lock de *otro* cerrojo, asi
		 * que o lo vemos aqui o su wake_up_process() nos encuentra ya
		 * fuera de TASK_RUNNING.
		 */
		set_current_state(state);
		if (READ_ONCE(w.granted))
			break;
		o = atomic_long_read(&m->owner);
		if (o == 0) {
			if (atomic_long_try_cmpxchg(&m->owner, &o, (long)me)) {
				if (queued)
					list_del(&w.list);
				break;
			}
			continue;
		}
		if (ww && may_die) {
			ret = __lkpi_ww_check(ww, ctx);
			if (ret)
				break;
		}
		if (signal_pending_state(state, me)) {
			ret = -EINTR;
			break;
		}
		if (!queued) {
			list_add_tail(&w.list, &m->wait_list);
			queued = true;
		}
		/* Con la marca puesta, mutex_unlock() va por el camino lento. */
		if (!(o & LKPI_MUTEX_WAITERS) &&
		    !atomic_long_try_cmpxchg(&m->owner, &o, o | LKPI_MUTEX_WAITERS))
			continue;
		if (ww && ctx && !ctx->is_wait_die)
			__lkpi_ww_wound(ww, ctx);
		raw_spin_unlock_irqrestore(&m->wait_lock, flags);
		schedule();
		raw_spin_lock_irqsave(&m->wait_lock, flags);
	}
	__set_current_state(TASK_RUNNING);
	if (ret) {
		if (queued)
			list_del(&w.list);
	} else {
		/* Dueno: la marca refleja si quedan esperas. */
		atomic_long_set(&m->owner, (long)me |
				(list_empty(&m->wait_list) ? 0 : LKPI_MUTEX_WAITERS));
		if (ww)
			__lkpi_ww_acquired_locked(ww, ctx);
	}
	raw_spin_unlock_irqrestore(&m->wait_lock, flags);
	return ret;
}

void lkpi_mutex_lock(struct mutex *m)
{
	if (!lkpi_mutex_trylock(m))
		__lkpi_mutex_lock_slow(m, TASK_UNINTERRUPTIBLE, NULL, NULL, false);
}
EXPORT_SYMBOL(lkpi_mutex_lock);

int lkpi_mutex_lock_interruptible(struct mutex *m)
{
	if (lkpi_mutex_trylock(m))
		return 0;
	return __lkpi_mutex_lock_slow(m, TASK_INTERRUPTIBLE, NULL, NULL, false);
}
EXPORT_SYMBOL(lkpi_mutex_lock_interruptible);

int lkpi_mutex_lock_killable(struct mutex *m)
{
	if (lkpi_mutex_trylock(m))
		return 0;
	return __lkpi_mutex_lock_slow(m, TASK_KILLABLE, NULL, NULL, false);
}
EXPORT_SYMBOL(lkpi_mutex_lock_killable);

void lkpi_mutex_unlock(struct mutex *m)
{
	long o = (long)current;
	struct task_struct *next = NULL;
	unsigned long flags;

	if (atomic_long_try_cmpxchg(&m->owner, &o, 0))
		return;
	raw_spin_lock_irqsave(&m->wait_lock, flags);
	if (list_empty(&m->wait_list)) {
		atomic_long_set(&m->owner, 0);
	} else {
		struct lkpi_mutex_waiter *w =
			list_first_entry(&m->wait_list, struct lkpi_mutex_waiter, list);

		list_del(&w->list);
		next = w->task;
		atomic_long_set(&m->owner, (long)next |
				(list_empty(&m->wait_list) ? 0 : LKPI_MUTEX_WAITERS));
		/* Tras esto `w` (en la pila del otro) puede desaparecer. */
		smp_store_release(&w->granted, true);
		wake_up_process(next);
	}
	raw_spin_unlock_irqrestore(&m->wait_lock, flags);
}
EXPORT_SYMBOL(lkpi_mutex_unlock);

/* ------------------------------------------------------------- ww_mutex */

/* `a` es mas viejo que `b` (sellos crecientes por clase). */
static inline bool __lkpi_ww_older(struct ww_acquire_ctx *a, struct ww_acquire_ctx *b)
{
	return (long)(a->stamp - b->stamp) < 0 || (a->stamp == b->stamp && a < b);
}

void lkpi_ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class)
{
	ctx->task = current;
	ctx->stamp = atomic_long_inc_return(&ww_class->stamp);
	ctx->acquired = 0;
	ctx->wounded = 0;
	ctx->is_wait_die = ww_class->is_wait_die;
	ctx->ww_class = ww_class;
}
EXPORT_SYMBOL(lkpi_ww_acquire_init);

/* Con wait_lock: ¿debe morir `ctx` ahora? Solo si ya tiene cerrojos. */
static int __lkpi_ww_check(struct ww_mutex *ww, struct ww_acquire_ctx *ctx)
{
	struct ww_acquire_ctx *hold = ww->ctx;

	if (!ctx || !ctx->acquired)
		return 0;
	if (ctx->is_wait_die)
		return (hold && hold != ctx && __lkpi_ww_older(hold, ctx)) ? -EDEADLK : 0;
	return READ_ONCE(ctx->wounded) ? -EDEADLK : 0;
}

/*
 * Wound-wait, con wait_lock: si el dueno es mas joven, se le marca herido y
 * se le despierta (si espera otro cerrojo, al despertar recibe -EDEADLK).
 * ww->ctx solo cambia bajo wait_lock, asi que `hold` sigue vivo.
 */
static void __lkpi_ww_wound(struct ww_mutex *ww, struct ww_acquire_ctx *ctx)
{
	struct ww_acquire_ctx *hold = ww->ctx;

	if (hold && hold != ctx && __lkpi_ww_older(ctx, hold) && !READ_ONCE(hold->wounded)) {
		WRITE_ONCE(hold->wounded, 1);
		if (hold->task != current)
			wake_up_process(hold->task);
	}
}

/*
 * Con wait_lock y el mutex base ya nuestro: publica el contexto y despierta
 * a todas las esperas para que reevaluen (morir o herir al nuevo dueno).
 */
static void __lkpi_ww_acquired_locked(struct ww_mutex *ww, struct ww_acquire_ctx *ctx)
{
	struct lkpi_mutex_waiter *w;

	ww->ctx = ctx;
	if (ctx)
		ctx->acquired++;
	list_for_each_entry(w, &ww->base.wait_list, list)
		wake_up_process(w->task);
}

static int __lkpi_ww_lock(struct ww_mutex *ww, struct ww_acquire_ctx *ctx, unsigned int state,
			  bool may_die)
{
	unsigned long flags;

	if (ctx) {
		if (READ_ONCE(ww->ctx) == ctx)
			return -EALREADY;
		if (!ctx->acquired)
			ctx->wounded = 0;
		/* Herido y con cerrojos: hay que soltar antes de esperar nada. */
		if (may_die && !ctx->is_wait_die && READ_ONCE(ctx->wounded))
			return -EDEADLK;
	}
	if (lkpi_mutex_trylock(&ww->base)) {
		raw_spin_lock_irqsave(&ww->base.wait_lock, flags);
		__lkpi_ww_acquired_locked(ww, ctx);
		raw_spin_unlock_irqrestore(&ww->base.wait_lock, flags);
		return 0;
	}
	return __lkpi_mutex_lock_slow(&ww->base, state, ww, ctx, may_die);
}

int lkpi_ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __lkpi_ww_lock(lock, ctx, TASK_UNINTERRUPTIBLE, true);
}
EXPORT_SYMBOL(lkpi_ww_mutex_lock);

int lkpi_ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __lkpi_ww_lock(lock, ctx, TASK_INTERRUPTIBLE, true);
}
EXPORT_SYMBOL(lkpi_ww_mutex_lock_interruptible);

/* Tras -EDEADLK y soltar todo: espera sin morir (el contexto no tiene nada). */
void lkpi_ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	WARN_ON(ctx && ctx->acquired);
	__lkpi_ww_lock(lock, ctx, TASK_UNINTERRUPTIBLE, false);
}
EXPORT_SYMBOL(lkpi_ww_mutex_lock_slow);

int lkpi_ww_mutex_lock_slow_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	WARN_ON(ctx && ctx->acquired);
	return __lkpi_ww_lock(lock, ctx, TASK_INTERRUPTIBLE, false);
}
EXPORT_SYMBOL(lkpi_ww_mutex_lock_slow_interruptible);

int lkpi_ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	unsigned long flags;

	if (ctx && READ_ONCE(lock->ctx) == ctx)
		return 0;
	if (!lkpi_mutex_trylock(&lock->base))
		return 0;
	raw_spin_lock_irqsave(&lock->base.wait_lock, flags);
	__lkpi_ww_acquired_locked(lock, ctx);
	raw_spin_unlock_irqrestore(&lock->base.wait_lock, flags);
	return 1;
}
EXPORT_SYMBOL(lkpi_ww_mutex_trylock);

void lkpi_ww_mutex_unlock(struct ww_mutex *lock)
{
	struct lkpi_mutex_waiter *w;
	unsigned long flags;

	raw_spin_lock_irqsave(&lock->base.wait_lock, flags);
	if (lock->ctx) {
		if (lock->ctx->acquired)
			lock->ctx->acquired--;
		lock->ctx = NULL;
	}
	/* Las demas esperas reevaluan contra el siguiente dueno. */
	list_for_each_entry(w, &lock->base.wait_list, list)
		wake_up_process(w->task);
	raw_spin_unlock_irqrestore(&lock->base.wait_lock, flags);
	lkpi_mutex_unlock(&lock->base);
}
EXPORT_SYMBOL(lkpi_ww_mutex_unlock);

/* ---------------------------------------------------------------- rwsem */

struct lkpi_rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	bool write;
	bool granted;
};

static void __lkpi_rwsem_grant(struct lkpi_rwsem_waiter *w)
{
	struct task_struct *t = w->task;

	list_del(&w->list);
	smp_store_release(&w->granted, true);
	wake_up_process(t);
}

/* Con wait_lock: concede a la cabeza (un escritor o los lectores seguidos). */
static void __lkpi_rwsem_wake(struct rw_semaphore *s)
{
	while (!list_empty(&s->wait_list)) {
		struct lkpi_rwsem_waiter *w =
			list_first_entry(&s->wait_list, struct lkpi_rwsem_waiter, list);

		if (w->write) {
			if (s->count == 0) {
				s->count = -1;
				__lkpi_rwsem_grant(w);
			}
			return;
		}
		if (s->count < 0)
			return;
		s->count++;
		__lkpi_rwsem_grant(w);
	}
}

static int __lkpi_rwsem_down(struct rw_semaphore *s, unsigned int state, bool write)
{
	struct lkpi_rwsem_waiter w = { .task = current, .write = write, .granted = false };
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&s->wait_lock, flags);
	if (list_empty(&s->wait_list) && (write ? s->count == 0 : s->count >= 0)) {
		s->count = write ? -1 : s->count + 1;
		raw_spin_unlock_irqrestore(&s->wait_lock, flags);
		return 0;
	}
	list_add_tail(&w.list, &s->wait_list);
	for (;;) {
		if (READ_ONCE(w.granted))
			break;
		if (signal_pending_state(state, current)) {
			list_del(&w.list);
			/* Quizas desbloqueaba a los de detras (p. ej. lectores tras un escritor). */
			__lkpi_rwsem_wake(s);
			ret = -EINTR;
			break;
		}
		set_current_state(state);
		raw_spin_unlock_irqrestore(&s->wait_lock, flags);
		schedule();
		raw_spin_lock_irqsave(&s->wait_lock, flags);
	}
	__set_current_state(TASK_RUNNING);
	raw_spin_unlock_irqrestore(&s->wait_lock, flags);
	return ret;
}

int lkpi_down_read_state(struct rw_semaphore *s, unsigned int state)
{
	return __lkpi_rwsem_down(s, state, false);
}
EXPORT_SYMBOL(lkpi_down_read_state);

int lkpi_down_write_state(struct rw_semaphore *s, unsigned int state)
{
	return __lkpi_rwsem_down(s, state, true);
}
EXPORT_SYMBOL(lkpi_down_write_state);

int lkpi_down_read_trylock(struct rw_semaphore *s)
{
	unsigned long flags;
	int ok;

	raw_spin_lock_irqsave(&s->wait_lock, flags);
	ok = s->count >= 0 && list_empty(&s->wait_list);
	if (ok)
		s->count++;
	raw_spin_unlock_irqrestore(&s->wait_lock, flags);
	return ok;
}
EXPORT_SYMBOL(lkpi_down_read_trylock);

int lkpi_down_write_trylock(struct rw_semaphore *s)
{
	unsigned long flags;
	int ok;

	raw_spin_lock_irqsave(&s->wait_lock, flags);
	ok = s->count == 0;
	if (ok)
		s->count = -1;
	raw_spin_unlock_irqrestore(&s->wait_lock, flags);
	return ok;
}
EXPORT_SYMBOL(lkpi_down_write_trylock);

void lkpi_up_read(struct rw_semaphore *s)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&s->wait_lock, flags);
	WARN_ON(s->count <= 0);
	if (--s->count == 0)
		__lkpi_rwsem_wake(s);
	raw_spin_unlock_irqrestore(&s->wait_lock, flags);
}
EXPORT_SYMBOL(lkpi_up_read);

void lkpi_up_write(struct rw_semaphore *s)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&s->wait_lock, flags);
	WARN_ON(s->count != -1);
	s->count = 0;
	__lkpi_rwsem_wake(s);
	raw_spin_unlock_irqrestore(&s->wait_lock, flags);
}
EXPORT_SYMBOL(lkpi_up_write);

void lkpi_downgrade_write(struct rw_semaphore *s)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&s->wait_lock, flags);
	WARN_ON(s->count != -1);
	s->count = 1;
	__lkpi_rwsem_wake(s);
	raw_spin_unlock_irqrestore(&s->wait_lock, flags);
}
EXPORT_SYMBOL(lkpi_downgrade_write);

/* ------------------------------------------------------------ semaphore */

struct lkpi_sem_waiter {
	struct list_head list;
	struct task_struct *task;
	bool granted;
};

int lkpi_down_state_timeout(struct semaphore *sem, unsigned int state, long timeout)
{
	struct lkpi_sem_waiter w = { .task = current, .granted = false };
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&sem->lock, flags);
	if (sem->count > 0) {
		sem->count--;
		raw_spin_unlock_irqrestore(&sem->lock, flags);
		return 0;
	}
	list_add_tail(&w.list, &sem->wait_list);
	for (;;) {
		if (READ_ONCE(w.granted))
			break;
		if (signal_pending_state(state, current)) {
			ret = -EINTR;
			break;
		}
		if (timeout <= 0) {
			ret = -ETIME;
			break;
		}
		set_current_state(state);
		raw_spin_unlock_irqrestore(&sem->lock, flags);
		timeout = schedule_timeout(timeout);
		raw_spin_lock_irqsave(&sem->lock, flags);
	}
	__set_current_state(TASK_RUNNING);
	if (ret)
		list_del(&w.list);
	raw_spin_unlock_irqrestore(&sem->lock, flags);
	return ret;
}
EXPORT_SYMBOL(lkpi_down_state_timeout);

/* Como en Linux: 0 si lo consiguio, 1 si no. */
int lkpi_down_trylock(struct semaphore *sem)
{
	unsigned long flags;
	int busy;

	raw_spin_lock_irqsave(&sem->lock, flags);
	busy = sem->count == 0;
	if (!busy)
		sem->count--;
	raw_spin_unlock_irqrestore(&sem->lock, flags);
	return busy;
}
EXPORT_SYMBOL(lkpi_down_trylock);

void lkpi_up(struct semaphore *sem)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&sem->lock, flags);
	if (list_empty(&sem->wait_list)) {
		sem->count++;
	} else {
		struct lkpi_sem_waiter *w =
			list_first_entry(&sem->wait_list, struct lkpi_sem_waiter, list);
		struct task_struct *t = w->task;

		list_del(&w->list);
		smp_store_release(&w->granted, true);
		wake_up_process(t);
	}
	raw_spin_unlock_irqrestore(&sem->lock, flags);
}
EXPORT_SYMBOL(lkpi_up);

/* ------------------------------------------------------------- refcount */

void lkpi_refcount_warn_saturate(refcount_t *r, int why)
{
	refcount_set(r, REFCOUNT_SATURATED);
	WARN_ONCE(1, "refcount_t: %s; se satura y el objeto se pierde\n",
		  why == __LKPI_RC_ADD_UAF ? "incremento desde 0 (uso tras liberar)" :
		  why == __LKPI_RC_ADD_OVF ? "desbordamiento" : "decremento por debajo de 0");
}
EXPORT_SYMBOL(lkpi_refcount_warn_saturate);

bool lkpi_refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock)
{
	if (refcount_dec_not_one(r))
		return false;
	mutex_lock(lock);
	if (!refcount_dec_and_test(r)) {
		mutex_unlock(lock);
		return false;
	}
	return true;
}
EXPORT_SYMBOL(lkpi_refcount_dec_and_mutex_lock);

/* ------------------------------------------------------------ notifier */

int lkpi_notifier_chain_register(struct notifier_block **nl, struct notifier_block *n)
{
	while (*nl) {
		if (WARN(*nl == n, "notifier registrado dos veces\n"))
			return -EEXIST;
		if (n->priority > (*nl)->priority)
			break;
		nl = &(*nl)->next;
	}
	n->next = *nl;
	rcu_assign_pointer(*nl, n);
	return 0;
}
EXPORT_SYMBOL(lkpi_notifier_chain_register);

int lkpi_notifier_chain_unregister(struct notifier_block **nl, struct notifier_block *n)
{
	while (*nl) {
		if (*nl == n) {
			rcu_assign_pointer(*nl, n->next);
			return 0;
		}
		nl = &(*nl)->next;
	}
	return -ENOENT;
}
EXPORT_SYMBOL(lkpi_notifier_chain_unregister);

int lkpi_notifier_call_chain(struct notifier_block **nl, unsigned long val, void *v)
{
	struct notifier_block *nb = rcu_dereference_raw(*nl), *next;
	int ret = NOTIFY_DONE;

	while (nb) {
		next = rcu_dereference_raw(nb->next);
		ret = nb->notifier_call(nb, val, v);
		if (ret & NOTIFY_STOP_MASK)
			break;
		nb = next;
	}
	return ret;
}
EXPORT_SYMBOL(lkpi_notifier_call_chain);

int lkpi_atomic_notifier_chain_register(struct atomic_notifier_head *nh, struct notifier_block *n)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&nh->lock, flags);
	ret = lkpi_notifier_chain_register(&nh->head, n);
	spin_unlock_irqrestore(&nh->lock, flags);
	return ret;
}
EXPORT_SYMBOL(lkpi_atomic_notifier_chain_register);

int lkpi_atomic_notifier_chain_unregister(struct atomic_notifier_head *nh,
					  struct notifier_block *n)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&nh->lock, flags);
	ret = lkpi_notifier_chain_unregister(&nh->head, n);
	spin_unlock_irqrestore(&nh->lock, flags);
	synchronize_rcu();
	return ret;
}
EXPORT_SYMBOL(lkpi_atomic_notifier_chain_unregister);

int lkpi_atomic_notifier_call_chain(struct atomic_notifier_head *nh, unsigned long val, void *v)
{
	int ret;

	rcu_read_lock();
	ret = lkpi_notifier_call_chain(&nh->head, val, v);
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL(lkpi_atomic_notifier_call_chain);

int lkpi_blocking_notifier_chain_register(struct blocking_notifier_head *nh,
					  struct notifier_block *n)
{
	int ret;

	down_write(&nh->rwsem);
	ret = lkpi_notifier_chain_register(&nh->head, n);
	up_write(&nh->rwsem);
	return ret;
}
EXPORT_SYMBOL(lkpi_blocking_notifier_chain_register);

int lkpi_blocking_notifier_chain_unregister(struct blocking_notifier_head *nh,
					    struct notifier_block *n)
{
	int ret;

	down_write(&nh->rwsem);
	ret = lkpi_notifier_chain_unregister(&nh->head, n);
	up_write(&nh->rwsem);
	return ret;
}
EXPORT_SYMBOL(lkpi_blocking_notifier_chain_unregister);

int lkpi_blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v)
{
	int ret = NOTIFY_DONE;

	if (rcu_access_pointer(nh->head)) {
		down_read(&nh->rwsem);
		ret = lkpi_notifier_call_chain(&nh->head, val, v);
		up_read(&nh->rwsem);
	}
	return ret;
}
EXPORT_SYMBOL(lkpi_blocking_notifier_call_chain);
