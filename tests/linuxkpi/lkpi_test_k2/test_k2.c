// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de K2 (L2): cerrojos, ww_mutex (-EDEADLK/-EALREADY y reintento),
 * esperas, completion, RCU/SRCU, tiempo, timers, hrtimers, workqueues,
 * tasklets, irq_work, kthreads, percpu y cpumask. Funcionan con una CPU; el
 * estres con varias esta en lkpi_test_k2_smp.
 *
 * Los plazos de Aurora tienen ~8 ms de resolucion: las comprobaciones de
 * tiempo solo exigen "no antes de" y un margen amplio por arriba (TCG).
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/local_lock.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/semaphore.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>
#include <linux/ww_mutex.h>
#include "lkpi_test.h"

#define KT_TIMEOUT (30 * HZ)	/* tope para no colgar la suite si algo falla */

/* --------------------------------------------- spinlocks y compania */

static DEFINE_SPINLOCK(kt_spin);
static DEFINE_RWLOCK(kt_rw);
static DEFINE_SEQLOCK(kt_seq);
static local_lock_t kt_llock = INIT_LOCAL_LOCK(kt_llock);

static void test_spinlocks(void)
{
	unsigned long flags;
	raw_spinlock_t raw;
	int saved = preempt_count();

	raw_spin_lock_init(&raw);
	spin_lock(&kt_spin);
	KT_ASSERT(spin_is_locked(&kt_spin), "spin_is_locked");
	KT_ASSERT(preempt_count() > saved, "spin_lock desactiva la expulsion");
	KT_EQ(spin_trylock(&kt_spin), 0);
	spin_unlock(&kt_spin);
	KT_EQ(preempt_count(), saved);

	spin_lock_irqsave(&kt_spin, flags);
	KT_ASSERT(irqs_disabled(), "spin_lock_irqsave desactiva las IRQ");
	spin_unlock_irqrestore(&kt_spin, flags);
	KT_ASSERT(!irqs_disabled(), "spin_unlock_irqrestore las restaura");

	KT_ASSERT(raw_spin_trylock_irqsave(&raw, flags), "raw trylock_irqsave");
	raw_spin_unlock_irqrestore(&raw, flags);
	spin_lock_bh(&kt_spin);
	spin_unlock_bh(&kt_spin);

	read_lock(&kt_rw);
	KT_EQ(read_trylock(&kt_rw), 1);		/* varios lectores */
	KT_EQ(write_trylock(&kt_rw), 0);
	read_unlock(&kt_rw);
	read_unlock(&kt_rw);
	write_lock_irqsave(&kt_rw, flags);
	KT_EQ(read_trylock(&kt_rw), 0);
	write_unlock_irqrestore(&kt_rw, flags);

	{
		unsigned int seq = read_seqbegin(&kt_seq);
		KT_EQ(read_seqretry(&kt_seq, seq), 0);
		write_seqlock(&kt_seq);
		write_sequnlock(&kt_seq);
		KT_EQ(read_seqretry(&kt_seq, seq), 1);
	}

	scoped_guard(spinlock_irqsave, &kt_spin)
		KT_ASSERT(irqs_disabled() && spin_is_locked(&kt_spin), "scoped_guard(spinlock_irqsave)");
	KT_ASSERT(!spin_is_locked(&kt_spin) && !irqs_disabled(), "guard soltado al salir");
	{
		guard(spinlock)(&kt_spin);
		KT_ASSERT(spin_is_locked(&kt_spin), "guard(spinlock)");
	}
	KT_ASSERT(!spin_is_locked(&kt_spin), "guard(spinlock) soltado");

	local_lock(&kt_llock);
	KT_ASSERT(preempt_count() > saved, "local_lock desactiva la expulsion");
	local_unlock(&kt_llock);
	lockdep_assert_held(&kt_spin);
	KT_EQ(preempt_count(), saved);
}

/* ------------------------------------------ atomic, refcount y kref */

static int kt_kref_released;
static void kt_kref_release(struct kref *k) { kt_kref_released++; }

static void test_refcounts(void)
{
	refcount_t r = REFCOUNT_INIT(1);
	struct kref k;
	atomic_t a = ATOMIC_INIT(5);

	refcount_inc(&r);
	KT_EQ(refcount_read(&r), 2);
	KT_ASSERT(!refcount_dec_and_test(&r), "2 -> 1");
	KT_ASSERT(refcount_dec_and_test(&r), "1 -> 0");
	KT_ASSERT(!refcount_inc_not_zero(&r), "inc_not_zero desde 0");
	refcount_set(&r, 1);
	KT_ASSERT(refcount_dec_if_one(&r), "dec_if_one");

	kref_init(&k);
	kref_get(&k);
	KT_EQ(kref_put(&k, kt_kref_release), 0);
	KT_EQ(kref_put(&k, kt_kref_release), 1);
	KT_EQ(kt_kref_released, 1);
	KT_EQ(kref_get_unless_zero(&k), 0);

	KT_ASSERT(atomic_dec_and_lock(&a, &kt_spin) == 0, "5 -> 4 sin cerrojo");
	atomic_set(&a, 1);
	KT_ASSERT(atomic_dec_and_lock(&a, &kt_spin) == 1, "1 -> 0 con cerrojo");
	spin_unlock(&kt_spin);
}

/* -------------------------------------------------------------- tiempo */

static void test_time(void)
{
	ktime_t t0, t1;
	unsigned long j0;
	s64 d, min = S64_MAX;
	int i;

	/* Resolucion fina (TSC) y monotono. */
	for (i = 0; i < 50; i++) {
		t0 = ktime_get();
		t1 = ktime_get();
		KT_ASSERT(t1 >= t0, "ktime_get monotono");
		if (t1 - t0 < min)
			min = t1 - t0;
	}
	KT_ASSERT(min < NSEC_PER_MSEC, "ktime_get con resolucion < 1 ms (min %lld ns)", min);
	KT_ASSERT(ktime_get_real_seconds() > 1600000000LL, "tiempo real (epoch)");
	KT_ASSERT(local_clock() > 0 && sched_clock() > 0, "local_clock/sched_clock");

	t0 = ktime_get();
	udelay(500);
	d = ktime_us_delta(ktime_get(), t0);
	KT_ASSERT(d >= 500 && d < 100000, "udelay(500) tardo %lld us", d);

	t0 = ktime_get();
	ndelay(20000);
	d = ktime_to_ns(ktime_sub(ktime_get(), t0));
	KT_ASSERT(d >= 20000, "ndelay(20000) tardo %lld ns", d);

	j0 = jiffies;
	msleep(20);
	KT_ASSERT(time_after_eq(jiffies, j0 + 20), "msleep(20): %lu jiffies", jiffies - j0);
	KT_ASSERT(time_before(jiffies, j0 + 2000), "msleep(20) acaba");

	t0 = ktime_get();
	usleep_range(300, 600);	/* corto: espera activa */
	KT_ASSERT(ktime_us_delta(ktime_get(), t0) >= 300, "usleep_range(300, 600)");
	j0 = jiffies;
	usleep_range(5000, 6000);
	KT_ASSERT(time_after_eq(jiffies, j0 + 5), "usleep_range(5000, 6000)");

	j0 = jiffies;
	KT_EQ(schedule_timeout_uninterruptible(10), 0);
	KT_ASSERT(time_after_eq(jiffies, j0 + 10), "schedule_timeout(10)");
	j0 = jiffies;
	set_current_state(TASK_RUNNING);
	KT_ASSERT(schedule_timeout(50) > 0 && time_before(jiffies, j0 + 50),
		  "schedule_timeout con TASK_RUNNING no duerme");
}

/* ---------------------------------------------------------- kthreads */

static DECLARE_COMPLETION(kt_thread_ran);
static atomic_t kt_thread_flag;

static int kt_thread_fn(void *data)
{
	atomic_set(&kt_thread_flag, 1);
	complete(&kt_thread_ran);
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return (int)(long)data;
}

static void test_kthreads(void)
{
	struct task_struct *t;

	t = kthread_create(kt_thread_fn, (void *)42L, "kt_k2_%d", 1);
	KT_ASSERT(!IS_ERR(t), "kthread_create");
	if (IS_ERR(t))
		return;
	KT_STREQ(t->comm, "kt_k2_1");
	KT_ASSERT(t->flags & PF_KTHREAD, "PF_KTHREAD");
	msleep(20);
	KT_EQ(atomic_read(&kt_thread_flag), 0);	/* no corre hasta despertarlo */
	KT_EQ(wake_up_process(t), 1);
	KT_ASSERT(wait_for_completion_timeout(&kt_thread_ran, KT_TIMEOUT), "el kthread arranca");
	KT_EQ(kthread_stop(t), 42);

	/* Parado sin haber arrancado: no llega a llamar a la funcion. */
	atomic_set(&kt_thread_flag, 0);
	t = kthread_create(kt_thread_fn, (void *)7L, "kt_k2_never");
	KT_ASSERT(!IS_ERR(t), "kthread_create (2)");
	if (!IS_ERR(t))
		KT_EQ(kthread_stop(t), -EINTR);
	KT_EQ(atomic_read(&kt_thread_flag), 0);

	t = kthread_run(kt_thread_fn, (void *)5L, "kt_k2_run");
	KT_ASSERT(!IS_ERR(t), "kthread_run");
	if (!IS_ERR(t)) {
		reinit_completion(&kt_thread_ran);
		KT_ASSERT(wait_for_completion_timeout(&kt_thread_ran, KT_TIMEOUT), "kthread_run arranca");
		KT_EQ(kthread_stop(t), 5);
	}
	KT_ASSERT(current != NULL && !(current->flags & PF_KTHREAD), "current del hilo de pruebas");
}

/* ------------------------------------------- wait_queue y completion */

static DECLARE_WAIT_QUEUE_HEAD(kt_wq);
static int kt_cond;
static struct completion kt_done;

static int kt_waker_fn(void *data)
{
	msleep(20);
	WRITE_ONCE(kt_cond, 1);
	wake_up(&kt_wq);
	msleep(20);
	complete(&kt_done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

static int kt_completion_waiter(void *data)
{
	struct completion *c = data;

	wait_for_completion(c);
	complete(&kt_done);
	return 0;
}

static void test_waits(void)
{
	struct task_struct *t, *w[3];
	unsigned long j0;
	long r;
	int i;

	init_completion(&kt_done);
	j0 = jiffies;
	KT_EQ(wait_event_timeout(kt_wq, READ_ONCE(kt_cond), 10), 0);
	KT_ASSERT(time_after_eq(jiffies, j0 + 10), "wait_event_timeout vence");
	KT_EQ(wait_event_interruptible_timeout(kt_wq, 1, 10), 10);	/* ya cierta */

	t = kthread_run(kt_waker_fn, NULL, "kt_waker");
	KT_ASSERT(!IS_ERR(t), "kthread waker");
	if (IS_ERR(t))
		return;
	r = wait_event_timeout(kt_wq, READ_ONCE(kt_cond), KT_TIMEOUT);
	KT_ASSERT(r > 0, "wait_event_timeout: despertado por wake_up (r=%ld)", r);
	KT_EQ(wait_for_completion_timeout(&kt_done, 0), 0);
	KT_ASSERT(wait_for_completion_timeout(&kt_done, KT_TIMEOUT) > 0, "complete() desde otro hilo");
	kthread_stop(t);

	/* complete_all libera a todos; completion_done/try_wait. */
	{
		static struct completion all;

		init_completion(&all);
		reinit_completion(&kt_done);
		KT_ASSERT(!completion_done(&all) && !try_wait_for_completion(&all), "no completada");
		for (i = 0; i < 3; i++)
			w[i] = kthread_run(kt_completion_waiter, &all, "kt_cw%d", i);
		msleep(20);
		complete_all(&all);
		for (i = 0; i < 3; i++)
			KT_ASSERT(wait_for_completion_timeout(&kt_done, KT_TIMEOUT), "waiter %d", i);
		for (i = 0; i < 3; i++)
			kthread_stop(w[i]);
		KT_ASSERT(completion_done(&all) && try_wait_for_completion(&all), "complete_all persiste");
		complete(&kt_done);
		KT_ASSERT(try_wait_for_completion(&kt_done), "complete + try_wait");
		KT_ASSERT(!try_wait_for_completion(&kt_done), "consumida");
		KT_EQ(wait_for_completion_interruptible_timeout(&kt_done, 3), 0);
	}

	/* Esperas sobre variables y bits. */
	{
		static unsigned long bits = 1;
		static int var;

		WRITE_ONCE(var, 1);
		wake_up_var(&var);
		wait_var_event(&var, READ_ONCE(var) == 1);
		clear_and_wake_up_bit(0, &bits);
		KT_EQ(wait_on_bit(&bits, 0, TASK_UNINTERRUPTIBLE), 0);
		KT_EQ(bits, 0);
	}
}

/* ------------------------------------------------------------- mutex */

static DEFINE_MUTEX(kt_mutex);
static unsigned long kt_mutex_counter;
static atomic_t kt_inside;

static int kt_mutex_fn(void *data)
{
	int i;

	for (i = 0; i < 300; i++) {
		mutex_lock(&kt_mutex);
		KT_ASSERT(atomic_inc_return(&kt_inside) == 1, "exclusion mutua del mutex");
		KT_ASSERT(mutex_get_owner(&kt_mutex) == (unsigned long)current, "propietario");
		kt_mutex_counter++;
		if (!(i % 50))
			msleep(1);	/* dormir con el mutex: otros esperan en la cola */
		atomic_dec(&kt_inside);
		mutex_unlock(&kt_mutex);
	}
	complete(&kt_done);
	return 0;
}

static void test_mutex(void)
{
	struct task_struct *t[4];
	int i;

	KT_ASSERT(!mutex_is_locked(&kt_mutex), "libre");
	KT_EQ(mutex_trylock(&kt_mutex), 1);
	KT_ASSERT(mutex_is_locked(&kt_mutex), "cogido");
	KT_EQ(mutex_trylock(&kt_mutex), 0);
	mutex_unlock(&kt_mutex);
	KT_EQ(mutex_lock_interruptible(&kt_mutex), 0);
	mutex_unlock(&kt_mutex);
	scoped_guard(mutex, &kt_mutex)
		KT_ASSERT(mutex_is_locked(&kt_mutex), "scoped_guard(mutex)");
	KT_ASSERT(!mutex_is_locked(&kt_mutex), "guard(mutex) suelto");

	init_completion(&kt_done);
	kt_mutex_counter = 0;
	for (i = 0; i < 4; i++)
		t[i] = kthread_run(kt_mutex_fn, NULL, "kt_mtx%d", i);
	for (i = 0; i < 4; i++)
		KT_ASSERT(wait_for_completion_timeout(&kt_done, KT_TIMEOUT), "hilo de mutex %d termina", i);
	for (i = 0; i < 4; i++)
		kthread_stop(t[i]);
	KT_EQ(kt_mutex_counter, 4 * 300);
}

/* ---------------------------------------------------------- ww_mutex */

static DEFINE_WD_CLASS(kt_wd_class);
static DEFINE_WW_CLASS(kt_ww_class);
static struct ww_mutex kt_wwa, kt_wwb;
static struct completion kt_young_has_b;
static int kt_young_first, kt_young_retry;

/* Wait-die en un solo hilo: el joven con cerrojos muere al chocar con el viejo. */
static void test_ww_wait_die(void)
{
	struct ww_acquire_ctx old, young;

	ww_mutex_init(&kt_wwa, &kt_wd_class);
	ww_mutex_init(&kt_wwb, &kt_wd_class);
	ww_acquire_init(&old, &kt_wd_class);
	ww_acquire_init(&young, &kt_wd_class);

	KT_EQ(ww_mutex_lock(&kt_wwa, &old), 0);
	KT_EQ(ww_mutex_lock(&kt_wwa, &old), -EALREADY);
	KT_EQ(ww_mutex_lock(&kt_wwb, &young), 0);
	KT_EQ(ww_mutex_lock(&kt_wwa, &young), -EDEADLK);	/* muere sin esperar */
	KT_EQ(young.acquired, 1);
	/* Reintento: suelta todo, espera el disputado con lock_slow y sigue. */
	ww_mutex_unlock(&kt_wwb);
	ww_mutex_unlock(&kt_wwa);			/* el viejo termina */
	ww_acquire_fini(&old);
	ww_mutex_lock_slow(&kt_wwa, &young);
	KT_EQ(ww_mutex_lock(&kt_wwb, &young), 0);
	KT_EQ(young.acquired, 2);
	ww_acquire_done(&young);
	KT_EQ(ww_mutex_trylock(&kt_wwa, NULL), 0);
	ww_mutex_unlock(&kt_wwa);
	ww_mutex_unlock(&kt_wwb);
	ww_acquire_fini(&young);
	KT_ASSERT(!ww_mutex_is_locked(&kt_wwa) && !ww_mutex_is_locked(&kt_wwb), "libres");
	KT_EQ(ww_mutex_trylock(&kt_wwa, NULL), 1);
	ww_mutex_unlock(&kt_wwa);
}

/*
 * Wound-wait con dos hilos: el joven (hilo) tiene B; el viejo (este hilo)
 * tiene A y pide B, asi que hiere al joven y espera. El joven pide A y
 * recibe -EDEADLK; suelta B (el viejo continua), espera A con lock_slow y
 * reintenta.
 */
static int kt_ww_young_fn(void *data)
{
	struct ww_acquire_ctx young;

	ww_acquire_init(&young, &kt_ww_class);
	ww_mutex_lock(&kt_wwb, &young);
	complete(&kt_young_has_b);
	msleep(50);
	kt_young_first = ww_mutex_lock(&kt_wwa, &young);
	if (kt_young_first == -EDEADLK) {
		/* Reintento: suelta B (el viejo sigue) y espera A sin morir. */
		ww_mutex_unlock(&kt_wwb);
		ww_mutex_lock_slow(&kt_wwa, &young);
		kt_young_retry = ww_mutex_lock(&kt_wwb, &young);
	}
	/* Aqui tiene A, y B salvo que el reintento fallara. */
	if (kt_young_first != -EDEADLK || !kt_young_retry)
		ww_mutex_unlock(&kt_wwb);
	ww_mutex_unlock(&kt_wwa);
	ww_acquire_fini(&young);
	complete(&kt_done);
	return 0;
}

static void test_ww_wound_wait(void)
{
	struct ww_acquire_ctx old;
	struct task_struct *t;

	ww_mutex_init(&kt_wwa, &kt_ww_class);
	ww_mutex_init(&kt_wwb, &kt_ww_class);
	init_completion(&kt_young_has_b);
	init_completion(&kt_done);
	kt_young_first = kt_young_retry = 1;

	ww_acquire_init(&old, &kt_ww_class);		/* sello menor: el viejo */
	KT_EQ(ww_mutex_lock(&kt_wwa, &old), 0);
	t = kthread_run(kt_ww_young_fn, NULL, "kt_ww_young");
	KT_ASSERT(!IS_ERR(t), "hilo joven");
	if (IS_ERR(t))
		return;
	KT_ASSERT(wait_for_completion_timeout(&kt_young_has_b, KT_TIMEOUT), "el joven tiene B");
	KT_EQ(ww_mutex_lock(&kt_wwb, &old), 0);	/* hiere al joven y espera */
	KT_EQ(old.acquired, 2);
	ww_mutex_unlock(&kt_wwb);
	ww_mutex_unlock(&kt_wwa);
	ww_acquire_fini(&old);
	KT_ASSERT(wait_for_completion_timeout(&kt_done, KT_TIMEOUT), "el joven termina");
	kthread_stop(t);
	KT_EQ(kt_young_first, -EDEADLK);
	KT_EQ(kt_young_retry, 0);
}

/* ------------------------------------------------- rwsem y semaphore */

static DECLARE_RWSEM(kt_rwsem);
static DEFINE_SEMAPHORE(kt_sem, 2);

static int kt_reader_fn(void *data)
{
	down_read(&kt_rwsem);
	complete(&kt_done);
	msleep(30);
	up_read(&kt_rwsem);
	return 0;
}

static void test_rwsem_sem(void)
{
	struct task_struct *t;
	unsigned long j0;

	down_read(&kt_rwsem);
	KT_EQ(down_read_trylock(&kt_rwsem), 1);
	KT_EQ(down_write_trylock(&kt_rwsem), 0);
	up_read(&kt_rwsem);
	up_read(&kt_rwsem);
	down_write(&kt_rwsem);
	KT_EQ(down_read_trylock(&kt_rwsem), 0);
	KT_ASSERT(rwsem_is_locked(&kt_rwsem), "rwsem_is_locked");
	downgrade_write(&kt_rwsem);
	KT_EQ(down_read_trylock(&kt_rwsem), 1);
	up_read(&kt_rwsem);
	up_read(&kt_rwsem);

	/* Un escritor espera a que salga un lector de otro hilo. */
	init_completion(&kt_done);
	t = kthread_run(kt_reader_fn, NULL, "kt_reader");
	KT_ASSERT(!IS_ERR(t), "hilo lector");
	if (IS_ERR(t))
		return;
	wait_for_completion(&kt_done);
	j0 = jiffies;
	down_write(&kt_rwsem);
	KT_ASSERT(time_after_eq(jiffies, j0 + 1), "down_write espero al lector");
	up_write(&kt_rwsem);
	kthread_stop(t);

	KT_EQ(down_trylock(&kt_sem), 0);
	KT_EQ(down_trylock(&kt_sem), 0);
	KT_EQ(down_trylock(&kt_sem), 1);	/* agotado */
	j0 = jiffies;
	KT_EQ(down_timeout(&kt_sem, 10), -ETIME);
	KT_ASSERT(time_after_eq(jiffies, j0 + 10), "down_timeout espera el plazo");
	up(&kt_sem);
	KT_EQ(down_interruptible(&kt_sem), 0);
	up(&kt_sem);
	up(&kt_sem);
}

/* ---------------------------------------------------------------- RCU */

struct kt_rcu_obj {
	int magic;
	struct rcu_head rcu;
	struct list_head node;
};

static struct kt_rcu_obj __rcu *kt_rcu_ptr;
static atomic_t kt_rcu_cbs;
static int kt_rcu_reader_done;
static DEFINE_SRCU(kt_srcu);

static void kt_rcu_cb(struct rcu_head *h)
{
	struct kt_rcu_obj *o = container_of(h, struct kt_rcu_obj, rcu);

	o->magic = 0xdead;
	kfree(o);
	atomic_inc(&kt_rcu_cbs);
}

static int kt_rcu_reader_fn(void *data)
{
	rcu_read_lock();
	complete(&kt_done);
	mdelay(40);	/* sin dormir: es una seccion de lectura RCU */
	WRITE_ONCE(kt_rcu_reader_done, 1);
	rcu_read_unlock();
	return 0;
}

static int kt_srcu_reader_fn(void *data)
{
	int idx = srcu_read_lock(&kt_srcu);

	complete(&kt_done);
	msleep(40);	/* SRCU permite dormir */
	WRITE_ONCE(kt_rcu_reader_done, 1);
	srcu_read_unlock(&kt_srcu, idx);
	return 0;
}

static void test_rcu(void)
{
	struct kt_rcu_obj *o, *old;
	struct task_struct *t;
	unsigned long cookie;
	LIST_HEAD(head);
	int i, n;

	o = kzalloc(sizeof(*o), GFP_KERNEL);
	o->magic = 1;
	rcu_assign_pointer(kt_rcu_ptr, o);
	rcu_read_lock();
	KT_EQ(rcu_dereference(kt_rcu_ptr)->magic, 1);
	rcu_read_unlock();

	cookie = get_state_synchronize_rcu();
	synchronize_rcu();
	KT_ASSERT(poll_state_synchronize_rcu(cookie), "un periodo de gracia completo");

	/* synchronize_rcu espera a un lector que ya estaba dentro. */
	init_completion(&kt_done);
	kt_rcu_reader_done = 0;
	t = kthread_run(kt_rcu_reader_fn, NULL, "kt_rcu_rd");
	if (!IS_ERR(t)) {
		wait_for_completion(&kt_done);
		synchronize_rcu();
		KT_EQ(READ_ONCE(kt_rcu_reader_done), 1);
		kthread_stop(t);
	}

	/* call_rcu y kfree_rcu, por lotes. */
	old = rcu_replace_pointer(kt_rcu_ptr, NULL, true);
	call_rcu(&old->rcu, kt_rcu_cb);
	for (i = 0; i < 20; i++) {
		o = kzalloc(sizeof(*o), GFP_KERNEL);
		if (i & 1)
			call_rcu(&o->rcu, kt_rcu_cb);
		else
			kfree_rcu(o, rcu);
	}
	kfree_rcu_mightsleep(kzalloc(32, GFP_KERNEL));
	rcu_barrier();
	KT_EQ(atomic_read(&kt_rcu_cbs), 11);

	/* Lista RCU. */
	for (i = 0; i < 5; i++) {
		o = kzalloc(sizeof(*o), GFP_KERNEL);
		o->magic = i;
		list_add_tail_rcu(&o->node, &head);
	}
	n = 0;
	rcu_read_lock();
	list_for_each_entry_rcu(o, &head, node)
		n += o->magic;
	rcu_read_unlock();
	KT_EQ(n, 0 + 1 + 2 + 3 + 4);
	while (!list_empty(&head)) {
		o = list_first_entry(&head, struct kt_rcu_obj, node);
		list_del_rcu(&o->node);
		kfree_rcu(o, rcu);
	}
	rcu_barrier();

	/* SRCU: synchronize_srcu espera a un lector que duerme. */
	init_completion(&kt_done);
	kt_rcu_reader_done = 0;
	t = kthread_run(kt_srcu_reader_fn, NULL, "kt_srcu_rd");
	if (!IS_ERR(t)) {
		wait_for_completion(&kt_done);
		synchronize_srcu(&kt_srcu);
		KT_EQ(READ_ONCE(kt_rcu_reader_done), 1);
		kthread_stop(t);
	}
	cleanup_srcu_struct(&kt_srcu);
}

/* ---------------------------------------------------- timers y hrtimers */

static struct timer_list kt_timer;
static atomic_t kt_timer_fired;
static unsigned long kt_timer_jiffies;
static bool kt_timer_in_softirq;

static void kt_timer_fn(struct timer_list *t)
{
	kt_timer_jiffies = jiffies;
	kt_timer_in_softirq = in_softirq() && preempt_count() > 0;
	if (atomic_inc_return(&kt_timer_fired) < 3 && t->expires)
		mod_timer(t, jiffies + 2);	/* se rearma dos veces */
}

static struct hrtimer kt_hrt;
static atomic_t kt_hrt_fired;
static ktime_t kt_hrt_at;

static enum hrtimer_restart kt_hrt_fn(struct hrtimer *t)
{
	kt_hrt_at = ktime_get();
	if (atomic_inc_return(&kt_hrt_fired) < 3) {
		hrtimer_forward_now(t, ms_to_ktime(5));
		return HRTIMER_RESTART;
	}
	return HRTIMER_NORESTART;
}

static void test_timers(void)
{
	unsigned long j0;
	ktime_t t0;
	int i;

	timer_setup(&kt_timer, kt_timer_fn, 0);
	KT_ASSERT(!timer_pending(&kt_timer), "no pendiente");
	j0 = jiffies;
	KT_EQ(mod_timer(&kt_timer, j0 + 15), 0);
	KT_ASSERT(timer_pending(&kt_timer), "pendiente");
	KT_EQ(mod_timer(&kt_timer, j0 + 10), 1);
	for (i = 0; i < 300 && atomic_read(&kt_timer_fired) < 3; i++)
		msleep(5);
	KT_EQ(atomic_read(&kt_timer_fired), 3);
	KT_ASSERT(time_after_eq(kt_timer_jiffies, j0 + 10), "el timer no se adelanta");
	KT_ASSERT(kt_timer_in_softirq, "funcion del timer en softirq con la expulsion desactivada");
	KT_EQ(del_timer_sync(&kt_timer), 0);

	/* Borrado antes de vencer: no se ejecuta. */
	atomic_set(&kt_timer_fired, 0);
	kt_timer.expires = 0;
	mod_timer(&kt_timer, jiffies + 20);
	KT_EQ(timer_delete_sync(&kt_timer), 1);
	msleep(40);
	KT_EQ(atomic_read(&kt_timer_fired), 0);
	timer_shutdown_sync(&kt_timer);
	mod_timer(&kt_timer, jiffies + 1);		/* ignorado tras shutdown */
	KT_ASSERT(!timer_pending(&kt_timer), "shutdown impide rearmar");

	/* hrtimer periodico con forward y restart. */
	hrtimer_setup(&kt_hrt, kt_hrt_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	t0 = ktime_get();
	hrtimer_start(&kt_hrt, ms_to_ktime(5), HRTIMER_MODE_REL);
	KT_ASSERT(hrtimer_active(&kt_hrt) && hrtimer_is_queued(&kt_hrt), "hrtimer en cola");
	for (i = 0; i < 300 && atomic_read(&kt_hrt_fired) < 3; i++)
		msleep(5);
	KT_EQ(atomic_read(&kt_hrt_fired), 3);
	KT_ASSERT(ktime_ms_delta(kt_hrt_at, t0) >= 15, "3 periodos de 5 ms: %lld ms",
		  ktime_ms_delta(kt_hrt_at, t0));
	msleep(20);
	KT_ASSERT(!hrtimer_active(&kt_hrt), "NORESTART: inactivo");
	hrtimer_start(&kt_hrt, ms_to_ktime(1000), HRTIMER_MODE_REL);
	KT_EQ(hrtimer_cancel(&kt_hrt), 1);
	KT_EQ(hrtimer_try_to_cancel(&kt_hrt), 0);
}

/* --------------------------------------------------------- workqueue */

static struct completion kt_gate, kt_entered;
static atomic_t kt_work_runs, kt_work_concurrent, kt_work_max_conc;
static struct work_struct kt_work, kt_work2;
static struct delayed_work kt_dwork;
static unsigned long kt_dwork_jiffies;
static int kt_order[64], kt_order_n;
static struct work_struct kt_order_works[64];
static struct work_struct *kt_seen_current;

static void kt_gated_fn(struct work_struct *w)
{
	int c = atomic_inc_return(&kt_work_concurrent);

	if (c > atomic_read(&kt_work_max_conc))
		atomic_set(&kt_work_max_conc, c);
	kt_seen_current = current_work();
	complete(&kt_entered);
	wait_for_completion(&kt_gate);
	atomic_inc(&kt_work_runs);
	atomic_dec(&kt_work_concurrent);
}

static void kt_count_fn(struct work_struct *w)
{
	atomic_inc(&kt_work_runs);
}

static void kt_dwork_fn(struct work_struct *w)
{
	kt_dwork_jiffies = jiffies;
	atomic_inc(&kt_work_runs);
}

static void kt_order_fn(struct work_struct *w)
{
	kt_order[kt_order_n++] = w - kt_order_works;
}

static void kt_sleepy_fn(struct work_struct *w)
{
	complete(&kt_entered);
	wait_for_completion(&kt_gate);
}

static struct rcu_work kt_rwork;
static void kt_rwork_fn(struct work_struct *w)
{
	atomic_inc(&kt_work_runs);
}

static void test_workqueues(void)
{
	struct workqueue_struct *wq, *owq;
	struct work_struct sleepy[3];
	unsigned long j0;
	int i, ok;

	wq = alloc_workqueue("kt_wq%d", WQ_UNBOUND, 0, 2);
	owq = alloc_ordered_workqueue("kt_owq", 0);
	KT_ASSERT(wq && owq, "alloc_workqueue");
	if (!wq || !owq)
		return;

	/* Reencolar mientras se ejecuta: vuelve a correr, y nunca en paralelo. */
	init_completion(&kt_gate);
	init_completion(&kt_entered);
	atomic_set(&kt_work_runs, 0);
	INIT_WORK(&kt_work, kt_gated_fn);
	KT_EQ(queue_work(wq, &kt_work), true);
	KT_ASSERT(wait_for_completion_timeout(&kt_entered, KT_TIMEOUT), "el trabajo empieza");
	KT_ASSERT(kt_seen_current == &kt_work, "current_work()");
	KT_ASSERT(!work_pending(&kt_work), "sin PENDING mientras corre");
	KT_EQ(queue_work(wq, &kt_work), true);	/* en curso: se reencola */
	KT_EQ(queue_work(wq, &kt_work), false);	/* ya pendiente */
	complete(&kt_gate);
	KT_ASSERT(wait_for_completion_timeout(&kt_entered, KT_TIMEOUT), "segunda ejecucion");
	complete(&kt_gate);
	flush_work(&kt_work);
	KT_EQ(atomic_read(&kt_work_runs), 2);
	KT_EQ(atomic_read(&kt_work_max_conc), 1);
	KT_EQ(flush_work(&kt_work), false);		/* nada que esperar */

	/* cancel_work_sync de un trabajo que espera detras de otro (cola ordenada). */
	reinit_completion(&kt_gate);
	reinit_completion(&kt_entered);
	atomic_set(&kt_work_runs, 0);
	INIT_WORK(&kt_work2, kt_count_fn);
	queue_work(owq, &kt_work);
	wait_for_completion(&kt_entered);
	queue_work(owq, &kt_work2);
	KT_EQ(cancel_work_sync(&kt_work2), true);
	KT_EQ(cancel_work_sync(&kt_work2), false);
	complete(&kt_gate);
	flush_workqueue(owq);
	KT_EQ(atomic_read(&kt_work_runs), 1);		/* solo kt_work */

	/* Orden FIFO en una cola ordenada. */
	kt_order_n = 0;
	for (i = 0; i < 40; i++) {
		INIT_WORK(&kt_order_works[i], kt_order_fn);
		queue_work(owq, &kt_order_works[i]);
	}
	flush_workqueue(owq);
	ok = kt_order_n == 40;
	for (i = 0; i < kt_order_n; i++)
		ok &= kt_order[i] == i;
	KT_ASSERT(ok, "cola ordenada en orden (%d trabajos)", kt_order_n);

	/* delayed_work: no antes de tiempo; cancelar; mod; flush. */
	atomic_set(&kt_work_runs, 0);
	INIT_DELAYED_WORK(&kt_dwork, kt_dwork_fn);
	j0 = jiffies;
	KT_EQ(queue_delayed_work(wq, &kt_dwork, 20), true);
	KT_EQ(queue_delayed_work(wq, &kt_dwork, 20), false);
	KT_ASSERT(delayed_work_pending(&kt_dwork), "pendiente");
	for (i = 0; i < 300 && !atomic_read(&kt_work_runs); i++)
		msleep(5);
	KT_EQ(atomic_read(&kt_work_runs), 1);
	KT_ASSERT(time_after_eq(kt_dwork_jiffies, j0 + 20), "delayed_work no se adelanta");
	schedule_delayed_work(&kt_dwork, 1000);
	KT_EQ(cancel_delayed_work_sync(&kt_dwork), true);
	KT_EQ(mod_delayed_work(wq, &kt_dwork, 1000), false);
	KT_EQ(mod_delayed_work(wq, &kt_dwork, 2000), true);
	flush_delayed_work(&kt_dwork);			/* corre ya */
	KT_EQ(atomic_read(&kt_work_runs), 2);
	KT_EQ(cancel_delayed_work(&kt_dwork), false);

	/* Trabajos que duermen no bloquean a los demas (el grupo crece). */
	reinit_completion(&kt_gate);
	reinit_completion(&kt_entered);
	atomic_set(&kt_work_runs, 0);
	for (i = 0; i < 3; i++) {
		INIT_WORK(&sleepy[i], kt_sleepy_fn);
		queue_work(system_unbound_wq, &sleepy[i]);
	}
	for (i = 0; i < 3; i++)
		KT_ASSERT(wait_for_completion_timeout(&kt_entered, KT_TIMEOUT),
			  "trabajo dormido %d en marcha", i);
	schedule_work(&kt_work2);
	flush_work(&kt_work2);
	KT_EQ(atomic_read(&kt_work_runs), 1);
	complete_all(&kt_gate);
	for (i = 0; i < 3; i++)
		flush_work(&sleepy[i]);

	/* rcu_work */
	atomic_set(&kt_work_runs, 0);
	INIT_RCU_WORK(&kt_rwork, kt_rwork_fn);
	KT_EQ(queue_rcu_work(wq, &kt_rwork), true);
	flush_rcu_work(&kt_rwork);
	KT_EQ(atomic_read(&kt_work_runs), 1);

	destroy_workqueue(owq);
	destroy_workqueue(wq);
}

/* ---------------------------------------------- tasklets e irq_work */

static atomic_t kt_tl_runs;
static struct tasklet_struct kt_tl;
static struct irq_work kt_iw;
static atomic_t kt_iw_runs;

static void kt_tl_fn(struct tasklet_struct *t)
{
	atomic_inc(&kt_tl_runs);
}

static void kt_iw_fn(struct irq_work *w)
{
	atomic_inc(&kt_iw_runs);
}

static void test_tasklets(void)
{
	int i;
	unsigned long flags;

	tasklet_setup(&kt_tl, kt_tl_fn);
	tasklet_schedule(&kt_tl);
	for (i = 0; i < 200 && !atomic_read(&kt_tl_runs); i++)
		msleep(2);
	KT_EQ(atomic_read(&kt_tl_runs), 1);

	tasklet_disable(&kt_tl);
	tasklet_schedule(&kt_tl);
	msleep(30);
	KT_EQ(atomic_read(&kt_tl_runs), 1);		/* desactivado: aplazado */
	tasklet_enable(&kt_tl);
	for (i = 0; i < 200 && atomic_read(&kt_tl_runs) < 2; i++)
		msleep(2);
	KT_EQ(atomic_read(&kt_tl_runs), 2);
	tasklet_kill(&kt_tl);

	init_irq_work(&kt_iw, kt_iw_fn);
	local_irq_save(flags);
	KT_EQ(irq_work_queue(&kt_iw), true);		/* con IRQ desactivadas */
	local_irq_restore(flags);
	irq_work_sync(&kt_iw);
	for (i = 0; i < 200 && !atomic_read(&kt_iw_runs); i++)
		msleep(2);
	KT_EQ(atomic_read(&kt_iw_runs), 1);
	KT_ASSERT(!irq_work_is_busy(&kt_iw), "irq_work terminado");
}

/* --------------------------------------------- CPUs, percpu, notifier */

static DEFINE_PER_CPU(int, kt_pcpu_counter);
static int kt_nb_calls;

static int kt_nb_fn(struct notifier_block *nb, unsigned long action, void *data)
{
	kt_nb_calls = kt_nb_calls * 10 + nb->priority;
	return action == 2 ? NOTIFY_STOP : NOTIFY_OK;
}

static void test_cpus(void)
{
	int *pc = alloc_percpu(int);
	int cpu, sum = 0, n = 0;
	struct notifier_block nb1 = { .notifier_call = kt_nb_fn, .priority = 1 };
	struct notifier_block nb2 = { .notifier_call = kt_nb_fn, .priority = 2 };
	BLOCKING_NOTIFIER_HEAD(bnh);
	ATOMIC_NOTIFIER_HEAD(anh);

	KT_ASSERT(num_online_cpus() >= 1 && num_online_cpus() <= nr_cpu_ids, "num_online_cpus = %u",
		  num_online_cpus());
	cpu = get_cpu();
	KT_ASSERT(cpu_online(cpu), "smp_processor_id %d en linea", cpu);
	this_cpu_inc(kt_pcpu_counter);
	KT_EQ(per_cpu(kt_pcpu_counter, cpu), 1);
	put_cpu();
	KT_ASSERT(pc != NULL, "alloc_percpu");
	if (pc) {
		for_each_online_cpu(cpu) {
			*per_cpu_ptr(pc, cpu) = cpu + 1;
			sum += cpu + 1;
			n++;
		}
		KT_EQ(n, num_online_cpus());
		for_each_possible_cpu(cpu)
			sum -= *per_cpu_ptr(pc, cpu);
		KT_EQ(sum, 0);
		free_percpu(pc);
	}
	KT_ASSERT(cpumask_test_cpu(0, cpumask_of(0)) && cpumask_weight(cpumask_of(3)) == 1,
		  "cpumask_of");

	blocking_notifier_chain_register(&bnh, &nb1);
	blocking_notifier_chain_register(&bnh, &nb2);
	kt_nb_calls = 0;
	KT_EQ(blocking_notifier_call_chain(&bnh, 1, NULL), NOTIFY_OK);
	KT_EQ(kt_nb_calls, 21);				/* por prioridad */
	kt_nb_calls = 0;
	KT_EQ(blocking_notifier_call_chain(&bnh, 2, NULL), NOTIFY_STOP);
	KT_EQ(kt_nb_calls, 2);
	atomic_notifier_chain_register(&anh, &nb1);
	kt_nb_calls = 0;
	atomic_notifier_call_chain(&anh, 1, NULL);
	KT_EQ(kt_nb_calls, 1);
	atomic_notifier_chain_unregister(&anh, &nb1);
	blocking_notifier_chain_unregister(&bnh, &nb1);
	blocking_notifier_chain_unregister(&bnh, &nb2);
}

static int __init test_k2_init(void)
{
	test_spinlocks();
	test_refcounts();
	test_time();
	test_kthreads();
	test_waits();
	test_mutex();
	test_ww_wait_die();
	test_ww_wound_wait();
	test_rwsem_sem();
	test_rcu();
	test_timers();
	test_workqueues();
	test_tasklets();
	test_cpus();
	rcu_barrier();	/* ningun callback de este modulo tras descargarlo */
	return 0;
}

static void __exit test_k2_exit(void)
{
}

module_init(test_k2_init);
module_exit(test_k2_exit);
MODULE_LICENSE("GPL");
