// SPDX-License-Identifier: GPL-2.0-only
/*
 * Estres de K2 con varias CPU (hito de L2): contencion de mutex y ww_mutex,
 * trabajo mas timers y RCU, sin interbloqueos. Cada fase tiene un tope de
 * tiempo: si un hilo se queda colgado la prueba falla (no cuelga la suite
 * esperando, aunque los hilos atascados no se pueden recoger).
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/ww_mutex.h>
#include "lkpi_test.h"

#define KT_PHASE_TIMEOUT (120 * HZ)
#define KT_MAX_THREADS 16

static int kt_nthreads;
static struct task_struct *kt_threads[KT_MAX_THREADS];
static struct completion kt_done;
static atomic_t kt_cpus_seen;		/* bit por CPU en la que corrio algun hilo */
static atomic_t kt_errors;

static void kt_note_cpu(void)
{
	atomic_or(1 << (raw_smp_processor_id() & 31), &kt_cpus_seen);
}

/* Lanza n hilos con fn(i), espera a que acaben (con tope) y los recoge. */
static bool kt_run_threads(int (*fn)(void *), int n, const char *name)
{
	bool ok = true;
	int i;

	init_completion(&kt_done);
	for (i = 0; i < n; i++)
		kt_threads[i] = kthread_run(fn, (void *)(long)i, "%s%d", name, i);
	for (i = 0; i < n; i++) {
		if (IS_ERR(kt_threads[i]))
			continue;
		if (!wait_for_completion_timeout(&kt_done, KT_PHASE_TIMEOUT)) {
			KT_ASSERT(0, "%s: hilos sin terminar (posible interbloqueo)", name);
			return false;
		}
	}
	for (i = 0; i < n; i++)
		if (!IS_ERR(kt_threads[i]))
			kthread_stop(kt_threads[i]);
		else
			ok = false;
	KT_ASSERT(ok, "%s: kthread_run", name);
	return ok;
}

/* ---------------------------------------------------------- mutex */

#define KT_MUTEX_ITERS 3000
static DEFINE_MUTEX(kt_mutex);
static unsigned long kt_mutex_counter;
static int kt_mutex_inside;

static int kt_mutex_fn(void *data)
{
	long id = (long)data;
	int i;

	kt_note_cpu();
	for (i = 0; i < KT_MUTEX_ITERS; i++) {
		if ((i + id) % 7 == 0)
			mutex_lock_interruptible(&kt_mutex);
		else
			mutex_lock(&kt_mutex);
		if (READ_ONCE(kt_mutex_inside))
			atomic_inc(&kt_errors);	/* otro hilo dentro a la vez */
		WRITE_ONCE(kt_mutex_inside, 1);
		kt_mutex_counter++;
		if (!(i % 97))
			udelay(20);
		if (!(i % 499))
			msleep(1);	/* dormir con el mutex */
		WRITE_ONCE(kt_mutex_inside, 0);
		mutex_unlock(&kt_mutex);
	}
	kt_note_cpu();
	complete(&kt_done);
	return 0;
}

static void stress_mutex(void)
{
	atomic_set(&kt_errors, 0);
	kt_mutex_counter = 0;
	if (!kt_run_threads(kt_mutex_fn, kt_nthreads, "kt_smtx"))
		return;
	KT_EQ(kt_mutex_counter, (unsigned long)kt_nthreads * KT_MUTEX_ITERS);
	KT_EQ(atomic_read(&kt_errors), 0);
}

/* ------------------------------------------------------- ww_mutex */

#define KT_WW_LOCKS 8
#define KT_WW_PICK 3
#define KT_WW_ITERS 400
static DEFINE_WW_CLASS(kt_ww_class);
static DEFINE_WD_CLASS(kt_wd_class);
static struct ww_mutex kt_ww[KT_WW_LOCKS];
static unsigned long kt_ww_count[KT_WW_LOCKS];
static int kt_ww_owner[KT_WW_LOCKS];
static atomic_t kt_ww_deadlk, kt_ww_rounds;
static struct ww_class *kt_ww_cur_class;

/* Toma los cerrojos idx[] en ese orden con el protocolo de reintento. */
static void kt_ww_take(struct ww_acquire_ctx *ctx, const int *idx, int n)
{
	struct ww_mutex *contended = NULL;
	int i, j, ret;

retry:
	for (i = 0; i < n; i++) {
		struct ww_mutex *l = &kt_ww[idx[i]];

		if (l == contended) {
			contended = NULL;	/* ya lo tenemos (lock_slow) */
			continue;
		}
		ret = ww_mutex_lock(l, ctx);
		if (ret == -EDEADLK) {
			atomic_inc(&kt_ww_deadlk);
			for (j = 0; j < i; j++)
				if (&kt_ww[idx[j]] != contended)
					ww_mutex_unlock(&kt_ww[idx[j]]);
			if (contended)
				ww_mutex_unlock(contended);
			ww_mutex_lock_slow(l, ctx);
			contended = l;
			/* El disputado va primero en la vuelta siguiente. */
			goto retry;
		}
		if (ret)
			atomic_inc(&kt_errors);
	}
}

static int kt_ww_fn(void *data)
{
	long id = (long)data;
	u64 seed = 0x9e3779b97f4a7c15ULL * (id + 1);
	int it, i, idx[KT_WW_PICK];

	kt_note_cpu();
	for (it = 0; it < KT_WW_ITERS; it++) {
		struct ww_acquire_ctx ctx;

		/* KT_WW_PICK cerrojos distintos en orden aleatorio. */
		for (i = 0; i < KT_WW_PICK; i++) {
			int j, dup;
			do {
				idx[i] = kt_rand(&seed) % KT_WW_LOCKS;
				for (dup = 0, j = 0; j < i; j++)
					dup |= idx[j] == idx[i];
			} while (dup);
		}
		ww_acquire_init(&ctx, kt_ww_cur_class);
		kt_ww_take(&ctx, idx, KT_WW_PICK);
		ww_acquire_done(&ctx);
		if (ctx.acquired != KT_WW_PICK)
			atomic_inc(&kt_errors);
		for (i = 0; i < KT_WW_PICK; i++) {
			/* Exclusion: nadie mas dentro de este cerrojo. */
			if (kt_ww_owner[idx[i]])
				atomic_inc(&kt_errors);
			kt_ww_owner[idx[i]] = id + 1;
			kt_ww_count[idx[i]]++;
		}
		if (!(it % 50))
			udelay(50);
		for (i = 0; i < KT_WW_PICK; i++) {
			kt_ww_owner[idx[i]] = 0;
			ww_mutex_unlock(&kt_ww[idx[i]]);
		}
		ww_acquire_fini(&ctx);
		atomic_inc(&kt_ww_rounds);
	}
	kt_note_cpu();
	complete(&kt_done);
	return 0;
}

static void stress_ww(struct ww_class *cls, const char *name)
{
	unsigned long total = 0;
	int i;

	kt_ww_cur_class = cls;
	for (i = 0; i < KT_WW_LOCKS; i++) {
		ww_mutex_init(&kt_ww[i], cls);
		kt_ww_count[i] = 0;
		kt_ww_owner[i] = 0;
	}
	atomic_set(&kt_errors, 0);
	atomic_set(&kt_ww_deadlk, 0);
	atomic_set(&kt_ww_rounds, 0);
	if (!kt_run_threads(kt_ww_fn, kt_nthreads, name))
		return;
	for (i = 0; i < KT_WW_LOCKS; i++) {
		total += kt_ww_count[i];
		KT_ASSERT(!ww_mutex_is_locked(&kt_ww[i]), "%s: cerrojo %d libre al final", name, i);
	}
	KT_EQ(total, (unsigned long)kt_nthreads * KT_WW_ITERS * KT_WW_PICK);
	KT_EQ(atomic_read(&kt_ww_rounds), kt_nthreads * KT_WW_ITERS);
	KT_EQ(atomic_read(&kt_errors), 0);
	pr_info("lkpi_test_k2_smp: %s: %d -EDEADLK con reintento en %d rondas\n", name,
		atomic_read(&kt_ww_deadlk), atomic_read(&kt_ww_rounds));
}

/* ------------------------------------------------ work + timers */

#define KT_WT_TIMERS 4
#define KT_WT_RUN_MS 1500
static struct workqueue_struct *kt_wq, *kt_owq;
static struct timer_list kt_wt_timer[KT_WT_TIMERS];
static struct work_struct kt_wt_work[KT_WT_TIMERS];
static struct delayed_work kt_wt_dwork[KT_WT_TIMERS];
static struct hrtimer kt_wt_hrt;
static atomic_t kt_wt_queued, kt_wt_ran, kt_wt_dqueued, kt_wt_dran, kt_wt_hrt_runs;
static atomic_t kt_wt_conc[KT_WT_TIMERS];
static bool kt_wt_stop;

static void kt_wt_work_fn(struct work_struct *w)
{
	int i = w - kt_wt_work;

	if (atomic_inc_return(&kt_wt_conc[i]) != 1)
		atomic_inc(&kt_errors);	/* reentrada */
	kt_note_cpu();
	if (!(atomic_read(&kt_wt_ran) % 13))
		udelay(100);
	atomic_inc(&kt_wt_ran);
	atomic_dec(&kt_wt_conc[i]);
}

static void kt_wt_dwork_fn(struct work_struct *w)
{
	atomic_inc(&kt_wt_dran);
	kt_note_cpu();
}

static void kt_wt_timer_fn(struct timer_list *t)
{
	int i = t - kt_wt_timer;

	/* Cada true de queue_work es exactamente una ejecucion. */
	if (queue_work(i & 1 ? kt_owq : kt_wq, &kt_wt_work[i]))
		atomic_inc(&kt_wt_queued);
	if (!mod_delayed_work(kt_wq, &kt_wt_dwork[i], 1 + (i & 3)))
		atomic_inc(&kt_wt_dqueued);
	if (!READ_ONCE(kt_wt_stop))
		mod_timer(t, jiffies + 1 + i);
}

static enum hrtimer_restart kt_wt_hrt_fn(struct hrtimer *t)
{
	atomic_inc(&kt_wt_hrt_runs);
	if (queue_work(system_unbound_wq, &kt_wt_work[0]))
		atomic_inc(&kt_wt_queued);
	if (READ_ONCE(kt_wt_stop))
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, us_to_ktime(700));
	return HRTIMER_RESTART;
}

/* Hilos que encolan a la vez que los timers. */
static int kt_wt_producer_fn(void *data)
{
	long id = (long)data;

	kt_note_cpu();
	while (!READ_ONCE(kt_wt_stop)) {
		int i = id % KT_WT_TIMERS;

		if (queue_work(kt_wq, &kt_wt_work[i]))
			atomic_inc(&kt_wt_queued);
		if (!(id & 1))
			flush_work(&kt_wt_work[i]);
		usleep_range(200, 400);
	}
	complete(&kt_done);
	return 0;
}

static void stress_work_timers(void)
{
	int i, ran_delayed;

	atomic_set(&kt_errors, 0);
	kt_wq = alloc_workqueue("kt_smp_wq", WQ_UNBOUND, 0);
	kt_owq = alloc_ordered_workqueue("kt_smp_owq", 0);
	KT_ASSERT(kt_wq && kt_owq, "alloc_workqueue");
	if (!kt_wq || !kt_owq)
		return;
	WRITE_ONCE(kt_wt_stop, false);
	for (i = 0; i < KT_WT_TIMERS; i++) {
		INIT_WORK(&kt_wt_work[i], kt_wt_work_fn);
		INIT_DELAYED_WORK(&kt_wt_dwork[i], kt_wt_dwork_fn);
		timer_setup(&kt_wt_timer[i], kt_wt_timer_fn, 0);
		mod_timer(&kt_wt_timer[i], jiffies + 1);
	}
	hrtimer_setup(&kt_wt_hrt, kt_wt_hrt_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&kt_wt_hrt, us_to_ktime(700), HRTIMER_MODE_REL);

	init_completion(&kt_done);
	for (i = 0; i < kt_nthreads; i++)
		kt_threads[i] = kthread_run(kt_wt_producer_fn, (void *)(long)i, "kt_prod%d", i);
	msleep(KT_WT_RUN_MS);
	WRITE_ONCE(kt_wt_stop, true);
	for (i = 0; i < kt_nthreads; i++)
		if (!IS_ERR(kt_threads[i]) &&
		    !wait_for_completion_timeout(&kt_done, KT_PHASE_TIMEOUT)) {
			KT_ASSERT(0, "productores sin terminar (posible interbloqueo)");
			return;
		}
	for (i = 0; i < kt_nthreads; i++)
		if (!IS_ERR(kt_threads[i]))
			kthread_stop(kt_threads[i]);

	/* Parar todo: timers (que rearman), luego el trabajo. */
	hrtimer_cancel(&kt_wt_hrt);
	for (i = 0; i < KT_WT_TIMERS; i++)
		timer_shutdown_sync(&kt_wt_timer[i]);
	for (i = 0; i < KT_WT_TIMERS; i++)
		flush_delayed_work(&kt_wt_dwork[i]);
	for (i = 0; i < KT_WT_TIMERS; i++)
		flush_work(&kt_wt_work[i]);
	flush_workqueue(kt_wq);
	flush_workqueue(kt_owq);
	flush_workqueue(system_unbound_wq);

	ran_delayed = atomic_read(&kt_wt_dran);
	KT_EQ(atomic_read(&kt_wt_ran), atomic_read(&kt_wt_queued));
	/* Cada paso "no pendiente -> pendiente" es una ejecucion (el flush final incluido). */
	KT_ASSERT(ran_delayed > 0 && ran_delayed == atomic_read(&kt_wt_dqueued),
		  "delayed_work: %d ejecuciones de %d armados", ran_delayed,
		  atomic_read(&kt_wt_dqueued));
	KT_ASSERT(atomic_read(&kt_wt_hrt_runs) > 10, "hrtimer periodico: %d",
		  atomic_read(&kt_wt_hrt_runs));
	KT_EQ(atomic_read(&kt_errors), 0);
	pr_info("lkpi_test_k2_smp: work+timers: %d trabajos, %d delayed, %d hrtimer\n",
		atomic_read(&kt_wt_ran), ran_delayed, atomic_read(&kt_wt_hrt_runs));
	destroy_workqueue(kt_owq);
	destroy_workqueue(kt_wq);
}

/* -------------------------------------------------------------- RCU */

#define KT_RCU_MAGIC 0x5a5a1234
#define KT_RCU_RUN_MS 1500
struct kt_rcu_obj {
	int magic;
	int gen;
	struct rcu_head rcu;
};
static struct kt_rcu_obj __rcu *kt_rcu_cur;
static atomic_t kt_rcu_reads, kt_rcu_freed, kt_rcu_queued;
static bool kt_rcu_stop;

static void kt_rcu_free_cb(struct rcu_head *h)
{
	struct kt_rcu_obj *o = container_of(h, struct kt_rcu_obj, rcu);

	o->magic = 0xdead;	/* un lector que aun lo viera lo notaria */
	kfree(o);
	atomic_inc(&kt_rcu_freed);
}

static int kt_rcu_reader_fn(void *data)
{
	kt_note_cpu();
	while (!READ_ONCE(kt_rcu_stop)) {
		struct kt_rcu_obj *o;
		int k;

		rcu_read_lock();
		o = rcu_dereference(kt_rcu_cur);
		for (k = 0; k < 20; k++)
			if (o && READ_ONCE(o->magic) != KT_RCU_MAGIC)
				atomic_inc(&kt_errors);
		rcu_read_unlock();
		atomic_inc(&kt_rcu_reads);
		if (!(atomic_read(&kt_rcu_reads) % 256))
			cond_resched();
	}
	complete(&kt_done);
	return 0;
}

static int kt_rcu_writer_fn(void *data)
{
	int gen = 0;

	kt_note_cpu();
	while (!READ_ONCE(kt_rcu_stop)) {
		struct kt_rcu_obj *n = kmalloc(sizeof(*n), GFP_KERNEL), *old;

		if (!n) {
			atomic_inc(&kt_errors);
			break;
		}
		n->magic = KT_RCU_MAGIC;
		n->gen = ++gen;
		old = rcu_replace_pointer(kt_rcu_cur, n, true);
		if (old) {
			atomic_inc(&kt_rcu_queued);
			if (gen % 3)
				call_rcu(&old->rcu, kt_rcu_free_cb);
			else {
				synchronize_rcu();
				kt_rcu_free_cb(&old->rcu);
			}
		}
		usleep_range(100, 200);
	}
	complete(&kt_done);
	return 0;
}

static void stress_rcu(void)
{
	int i, nreaders = max(kt_nthreads - 1, 2);
	struct kt_rcu_obj *last;

	atomic_set(&kt_errors, 0);
	WRITE_ONCE(kt_rcu_stop, false);
	init_completion(&kt_done);
	for (i = 0; i < nreaders; i++)
		kt_threads[i] = kthread_run(kt_rcu_reader_fn, NULL, "kt_rcur%d", i);
	kt_threads[nreaders] = kthread_run(kt_rcu_writer_fn, NULL, "kt_rcuw");
	msleep(KT_RCU_RUN_MS);
	WRITE_ONCE(kt_rcu_stop, true);
	for (i = 0; i <= nreaders; i++)
		if (!IS_ERR(kt_threads[i]) &&
		    !wait_for_completion_timeout(&kt_done, KT_PHASE_TIMEOUT)) {
			KT_ASSERT(0, "RCU: hilos sin terminar");
			return;
		}
	for (i = 0; i <= nreaders; i++)
		if (!IS_ERR(kt_threads[i]))
			kthread_stop(kt_threads[i]);
	rcu_barrier();
	last = rcu_replace_pointer(kt_rcu_cur, NULL, true);
	synchronize_rcu();
	kfree(last);
	KT_EQ(atomic_read(&kt_errors), 0);
	KT_EQ(atomic_read(&kt_rcu_freed), atomic_read(&kt_rcu_queued));
	KT_ASSERT(atomic_read(&kt_rcu_queued) > 20, "el escritor publico %d versiones",
		  atomic_read(&kt_rcu_queued));
	pr_info("lkpi_test_k2_smp: RCU: %d lecturas, %d versiones liberadas\n",
		atomic_read(&kt_rcu_reads), atomic_read(&kt_rcu_freed));
}

/* --------------------------------------------------- ping-pong */

#define KT_PP_ROUNDS 2000
static struct completion kt_ping, kt_pong;
static int kt_pp_fn(void *data)
{
	long side = (long)data;
	int i;

	kt_note_cpu();
	for (i = 0; i < KT_PP_ROUNDS; i++) {
		if (side) {
			if (!wait_for_completion_timeout(&kt_ping, KT_PHASE_TIMEOUT)) {
				atomic_inc(&kt_errors);
				break;
			}
			complete(&kt_pong);
		} else {
			complete(&kt_ping);
			if (!wait_for_completion_timeout(&kt_pong, KT_PHASE_TIMEOUT)) {
				atomic_inc(&kt_errors);
				break;
			}
		}
	}
	complete(&kt_done);
	return 0;
}

static void stress_pingpong(void)
{
	atomic_set(&kt_errors, 0);
	init_completion(&kt_ping);
	init_completion(&kt_pong);
	if (!kt_run_threads(kt_pp_fn, 2, "kt_pp"))
		return;
	KT_EQ(atomic_read(&kt_errors), 0);	/* ningun despertar perdido */
}

static int __init test_k2_smp_init(void)
{
	unsigned long j0 = jiffies;

	kt_nthreads = clamp_t(int, 2 * num_online_cpus(), 4, KT_MAX_THREADS - 1);
	KT_ASSERT(num_online_cpus() >= 2, "prueba SMP con %u CPUs", num_online_cpus());
	pr_info("lkpi_test_k2_smp: %u CPUs, %d hilos por fase\n", num_online_cpus(), kt_nthreads);
	stress_mutex();
	stress_ww(&kt_ww_class, "ww_mutex wound-wait");
	stress_ww(&kt_wd_class, "ww_mutex wait-die");
	stress_work_timers();
	stress_rcu();
	stress_pingpong();
	KT_ASSERT(hweight32(atomic_read(&kt_cpus_seen)) >= 2,
		  "los hilos corrieron en varias CPU (mascara 0x%x)", atomic_read(&kt_cpus_seen));
	pr_info("lkpi_test_k2_smp: CPUs usadas 0x%x, %u ms\n", atomic_read(&kt_cpus_seen),
		jiffies_to_msecs(jiffies - j0));
	rcu_barrier();
	return 0;
}

static void __exit test_k2_smp_exit(void)
{
}

module_init(test_k2_smp_init);
module_exit(test_k2_smp_exit);
MODULE_LICENSE("GPL");
