/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kthreads sobre los kthread de Aurora. Como en Linux, kthread_create()
 * deja el hilo dormido hasta el primer wake_up_process(); kthread_run() lo
 * crea y lo despierta. kthread_stop() pide parar, despierta, espera a que la
 * funcion vuelva y devuelve su resultado (-EINTR si nunca llego a correr).
 * Un hilo que termina solo, sin kthread_stop(), deja su descriptor sin
 * liberar hasta descargar la capa.
 */
#ifndef _LKPI_LINUX_KTHREAD_H
#define _LKPI_LINUX_KTHREAD_H

#include <linux/err.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>

struct task_struct *lkpi_kthread_create(int (*threadfn)(void *data), void *data,
					const char *namefmt, ...) __printf(3, 4);
int lkpi_kthread_stop(struct task_struct *k);
bool lkpi_kthread_should_stop(void);
void *lkpi_kthread_data(struct task_struct *k);

#define kthread_create(threadfn, data, namefmt, arg...) \
	lkpi_kthread_create(threadfn, data, namefmt, ##arg)
#define kthread_create_on_node(threadfn, data, node, namefmt, arg...) \
	lkpi_kthread_create(threadfn, data, namefmt, ##arg)
#define kthread_create_on_cpu(threadfn, data, cpu, namefmt) \
	lkpi_kthread_create(threadfn, data, namefmt, cpu)
#define kthread_run(threadfn, data, namefmt, ...) \
	({ struct task_struct *__k = lkpi_kthread_create(threadfn, data, namefmt, ##__VA_ARGS__); \
	   if (!IS_ERR(__k)) { wake_up_process(__k); } \
	   __k; })
#define kthread_stop(k) lkpi_kthread_stop(k)
#define kthread_stop_put(k) ({ int __r = lkpi_kthread_stop(k); __r; })
#define kthread_should_stop() lkpi_kthread_should_stop()
#define kthread_data(k) lkpi_kthread_data(k)
#define kthread_bind(k, cpu) do { (void)(k); (void)(cpu); } while (0)
#define kthread_bind_mask(k, mask) do { (void)(k); (void)(mask); } while (0)
#define kthread_should_park() false
#define kthread_parkme() do { } while (0)
#define kthread_freezable_should_stop(was_frozen) lkpi_kthread_should_stop()
#define kthread_use_mm(mm) do { (void)(mm); } while (0)
#define kthread_unuse_mm(mm) do { (void)(mm); } while (0)
#define set_freezable() do { } while (0)
#define try_to_freeze() false
#define freezing(p) false

/*
 * kthread_worker: un hilo que ejecuta trabajos encolados en orden. Como en
 * Linux, kthread_flush_work() espera a que el trabajo ni este encolado ni
 * corriendo. Implementacion en kernel/linuxkpi/kernel/kthread_worker.c.
 */
struct kthread_worker;
struct kthread_work;
typedef void (*kthread_work_func_t)(struct kthread_work *work);

struct kthread_worker {
	unsigned int flags;
	raw_spinlock_t lock;
	struct list_head work_list;
	struct list_head delayed_work_list;
	struct task_struct *task;
	struct kthread_work *current_work;
	wait_queue_head_t wake_wq;	/* hay trabajo o hay que parar */
	wait_queue_head_t idle_wq;	/* termino un trabajo */
	bool stopping;
};

struct kthread_work {
	struct list_head node;
	kthread_work_func_t func;
	struct kthread_worker *worker;
	int canceling;
};

struct kthread_delayed_work {
	struct kthread_work work;
	struct timer_list timer;
};

#define KTHREAD_WORK_INIT(work, fn) { .node = LIST_HEAD_INIT((work).node), .func = (fn) }
#define KTHREAD_WORKER_INIT(worker) { .work_list = LIST_HEAD_INIT((worker).work_list), \
				      .delayed_work_list = LIST_HEAD_INIT((worker).delayed_work_list) }

struct kthread_worker *lkpi_kthread_create_worker(unsigned int flags, const char *namefmt, ...);
void lkpi_kthread_destroy_worker(struct kthread_worker *worker);
bool lkpi_kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work);
bool lkpi_kthread_queue_delayed_work(struct kthread_worker *worker,
				     struct kthread_delayed_work *dwork, unsigned long delay);
bool lkpi_kthread_mod_delayed_work(struct kthread_worker *worker,
				   struct kthread_delayed_work *dwork, unsigned long delay);
void lkpi_kthread_flush_work(struct kthread_work *work);
void lkpi_kthread_flush_worker(struct kthread_worker *worker);
bool lkpi_kthread_cancel_work_sync(struct kthread_work *work);
bool lkpi_kthread_cancel_delayed_work_sync(struct kthread_delayed_work *dwork);
void lkpi_kthread_delayed_work_timer_fn(struct timer_list *t);

#define kthread_create_worker(flags, namefmt, ...) \
	lkpi_kthread_create_worker(flags, namefmt, ##__VA_ARGS__)
#define kthread_run_worker(flags, namefmt, ...) \
	lkpi_kthread_create_worker(flags, namefmt, ##__VA_ARGS__)
#define kthread_destroy_worker(w) lkpi_kthread_destroy_worker(w)
#define kthread_queue_work(w, work) lkpi_kthread_queue_work(w, work)
#define kthread_queue_delayed_work(w, dw, delay) lkpi_kthread_queue_delayed_work(w, dw, delay)
#define kthread_mod_delayed_work(w, dw, delay) lkpi_kthread_mod_delayed_work(w, dw, delay)
#define kthread_flush_work(work) lkpi_kthread_flush_work(work)
#define kthread_flush_worker(w) lkpi_kthread_flush_worker(w)
#define kthread_cancel_work_sync(work) lkpi_kthread_cancel_work_sync(work)
#define kthread_cancel_delayed_work_sync(dw) lkpi_kthread_cancel_delayed_work_sync(dw)

static inline void kthread_init_work(struct kthread_work *work, kthread_work_func_t fn)
{
	INIT_LIST_HEAD(&work->node);
	work->func = fn;
	work->worker = NULL;
	work->canceling = 0;
}

static inline void kthread_init_delayed_work(struct kthread_delayed_work *dwork,
					     kthread_work_func_t fn)
{
	kthread_init_work(&dwork->work, fn);
	timer_setup(&dwork->timer, lkpi_kthread_delayed_work_timer_fn, 0);
}

static inline void kthread_init_worker(struct kthread_worker *worker)
{
	memset(worker, 0, sizeof(*worker));
	raw_spin_lock_init(&worker->lock);
	INIT_LIST_HEAD(&worker->work_list);
	INIT_LIST_HEAD(&worker->delayed_work_list);
	init_waitqueue_head(&worker->wake_wq);
	init_waitqueue_head(&worker->idle_wq);
}

#define to_kthread_delayed_work(w) container_of(w, struct kthread_delayed_work, work)

#endif
