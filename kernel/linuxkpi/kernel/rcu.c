// SPDX-License-Identifier: GPL-2.0-only
/*
 * RCU (no expulsable) y SRCU (contadores por CPU).
 *
 * Periodo de gracia: foto de sched_rcu_qs_count() de cada CPU en linea (el
 * kernel lo incrementa en cada tick que llega con la expulsion activa, es
 * decir, fuera de rcu_read_lock()) y espera a que todas avancen. Los
 * periodos se serializan con un mutex; quien llega mientras otro corre y
 * ese otro empezo despues que el, lo aprovecha.
 *
 * call_rcu()/kfree_rcu(): una cola que vacia el hilo lkpi_rcu por lotes
 * (un periodo de gracia por lote).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/srcu.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

#define LKPI_RCU_STALL_JIFFIES (10 * HZ)

static DEFINE_MUTEX(lkpi_gp_mutex);
static unsigned long lkpi_gp_started;	/* periodos empezados (bajo el mutex) */
static unsigned long lkpi_gp_done;	/* periodos terminados */
static u64 lkpi_gp_snap[NR_CPUS];	/* bajo el mutex */

static void lkpi_rcu_run_gp(void)
{
	struct cpumask pending;
	unsigned long start = jiffies, next_warn = start + LKPI_RCU_STALL_JIFFIES;
	unsigned int cpu;

	smp_mb();	/* lo que el llamante retiro es visible antes de la foto */
	cpumask_copy(&pending, cpu_online_mask);
	for_each_cpu(cpu, &pending)
		lkpi_gp_snap[cpu] = aurora_sched_rcu_qs_count(cpu);
	for (;;) {
		for_each_cpu(cpu, &pending)
			if (aurora_sched_rcu_qs_count(cpu) != lkpi_gp_snap[cpu])
				cpumask_clear_cpu(cpu, &pending);
		if (cpumask_empty(&pending))
			break;
		if (time_after(jiffies, next_warn)) {
			cpu = cpumask_first(&pending);
			pr_warn("linuxkpi: periodo de gracia de RCU atascado %u s (CPU %u sin estado quiescente, contador %llu)\n",
				jiffies_to_msecs(jiffies - start) / 1000, cpu,
				aurora_sched_rcu_qs_count(cpu));
			next_warn = jiffies + LKPI_RCU_STALL_JIFFIES;
		}
		schedule_timeout_uninterruptible(1);
	}
	smp_mb();	/* los lectores anteriores terminaron antes de seguir */
}

void lkpi_synchronize_rcu(void)
{
	unsigned long seen = READ_ONCE(lkpi_gp_started);

	might_sleep();
	mutex_lock(&lkpi_gp_mutex);
	/* Un periodo que empezo y termino despues de entrar aqui sirve. */
	if ((long)(lkpi_gp_done - (seen + 1)) < 0) {
		WRITE_ONCE(lkpi_gp_started, lkpi_gp_started + 1);
		lkpi_rcu_run_gp();
		WRITE_ONCE(lkpi_gp_done, lkpi_gp_started);
	}
	mutex_unlock(&lkpi_gp_mutex);
}
EXPORT_SYMBOL(lkpi_synchronize_rcu);

unsigned long lkpi_get_state_synchronize_rcu(void)
{
	smp_mb();
	return READ_ONCE(lkpi_gp_started) + 1;
}
EXPORT_SYMBOL(lkpi_get_state_synchronize_rcu);

bool lkpi_poll_state_synchronize_rcu(unsigned long oldstate)
{
	bool done = (long)(READ_ONCE(lkpi_gp_done) - oldstate) >= 0;

	smp_mb();
	return done;
}
EXPORT_SYMBOL(lkpi_poll_state_synchronize_rcu);

/* ------------------------------------------------------------ call_rcu */

static DEFINE_SPINLOCK(lkpi_cb_lock);
static struct rcu_head *lkpi_cb_head;
static struct rcu_head **lkpi_cb_tail = &lkpi_cb_head;
static unsigned long lkpi_cb_queued;	/* callbacks encolados (bajo lkpi_cb_lock) */
static unsigned long lkpi_cb_invoked;	/* callbacks ya llamados */
static DECLARE_WAIT_QUEUE_HEAD(lkpi_cb_wq);
static struct task_struct *lkpi_rcu_task;

void lkpi_call_rcu(struct rcu_head *head, rcu_callback_t func)
{
	unsigned long flags;

	head->func = func;
	head->next = NULL;
	spin_lock_irqsave(&lkpi_cb_lock, flags);
	*lkpi_cb_tail = head;
	lkpi_cb_tail = &head->next;
	lkpi_cb_queued++;
	spin_unlock_irqrestore(&lkpi_cb_lock, flags);
	if (lkpi_rcu_task)
		wake_up_process(lkpi_rcu_task);
}
EXPORT_SYMBOL(lkpi_call_rcu);

/* La "funcion" de kfree_rcu es el desplazamiento de la cabeza en el objeto. */
void lkpi_kvfree_call_rcu(struct rcu_head *head, void *ptr)
{
	unsigned long off = (unsigned long)head - (unsigned long)ptr;

	if (WARN_ON(off >= 4096)) {
		synchronize_rcu();
		kvfree(ptr);
		return;
	}
	lkpi_call_rcu(head, (rcu_callback_t)off);
}
EXPORT_SYMBOL(lkpi_kvfree_call_rcu);

void lkpi_kvfree_rcu_mightsleep(void *ptr)
{
	if (!ptr)
		return;
	synchronize_rcu();
	kvfree(ptr);
}
EXPORT_SYMBOL(lkpi_kvfree_rcu_mightsleep);

static struct rcu_head *lkpi_cb_take(unsigned long *upto)
{
	struct rcu_head *list;
	unsigned long flags;

	spin_lock_irqsave(&lkpi_cb_lock, flags);
	list = lkpi_cb_head;
	lkpi_cb_head = NULL;
	lkpi_cb_tail = &lkpi_cb_head;
	*upto = lkpi_cb_queued;
	spin_unlock_irqrestore(&lkpi_cb_lock, flags);
	return list;
}

static void lkpi_cb_invoke(struct rcu_head *list, unsigned long upto)
{
	while (list) {
		struct rcu_head *next = list->next;
		unsigned long f = (unsigned long)list->func;

		if (f < 4096)
			kvfree((void *)((unsigned long)list - f));
		else
			list->func(list);
		list = next;
	}
	WRITE_ONCE(lkpi_cb_invoked, upto);
	wake_up_all(&lkpi_cb_wq);
}

static int lkpi_rcu_thread(void *unused)
{
	for (;;) {
		struct rcu_head *list;
		unsigned long upto;

		set_current_state(TASK_INTERRUPTIBLE);
		list = lkpi_cb_take(&upto);
		if (!list) {
			if (kthread_should_stop())
				break;
			schedule();
			continue;
		}
		__set_current_state(TASK_RUNNING);
		synchronize_rcu();
		lkpi_cb_invoke(list, upto);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

void lkpi_rcu_barrier(void)
{
	unsigned long flags, target;

	spin_lock_irqsave(&lkpi_cb_lock, flags);
	target = lkpi_cb_queued;
	spin_unlock_irqrestore(&lkpi_cb_lock, flags);
	wake_up_process(lkpi_rcu_task);
	wait_event(lkpi_cb_wq, (long)(READ_ONCE(lkpi_cb_invoked) - target) >= 0);
}
EXPORT_SYMBOL(lkpi_rcu_barrier);

/* ---------------------------------------------------------------- SRCU */

/* ¿Ningun lector con el indice `idx`? Ver linux/srcu.h. */
static bool lkpi_srcu_idx_idle(struct srcu_struct *ssp, unsigned int idx)
{
	unsigned long locks = 0, unlocks = 0;
	unsigned int cpu;

	for (cpu = 0; cpu < NR_CPUS; cpu++)
		unlocks += READ_ONCE(ssp->sda[cpu].unlock[idx]);
	smp_mb();	/* salidas antes que entradas: un lector visto salir se ve entrar */
	for (cpu = 0; cpu < NR_CPUS; cpu++)
		locks += READ_ONCE(ssp->sda[cpu].lock[idx]);
	return locks == unlocks;
}

static void lkpi_srcu_wait_idx(struct srcu_struct *ssp, unsigned int idx)
{
	while (!lkpi_srcu_idx_idle(ssp, idx))
		schedule_timeout_uninterruptible(1);
}

void lkpi_synchronize_srcu(struct srcu_struct *ssp)
{
	unsigned int cur;

	might_sleep();
	mutex_lock(&ssp->gp_mutex);
	smp_mb();	/* lo retirado se ve antes de mirar los contadores */
	cur = ssp->idx & 1;
	/* Rezagados de periodos anteriores que aun usan el otro indice. */
	lkpi_srcu_wait_idx(ssp, cur ^ 1);
	smp_mb();
	WRITE_ONCE(ssp->idx, cur ^ 1);
	smp_mb();	/* los lectores nuevos ya ven el indice nuevo */
	lkpi_srcu_wait_idx(ssp, cur);
	smp_mb();
	mutex_unlock(&ssp->gp_mutex);
}
EXPORT_SYMBOL(lkpi_synchronize_srcu);

struct lkpi_srcu_cb {
	struct work_struct work;
	struct srcu_struct *ssp;
	struct rcu_head *head;
	rcu_callback_t func;
};

static atomic_t lkpi_srcu_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkpi_srcu_wq);

static void lkpi_srcu_cb_fn(struct work_struct *work)
{
	struct lkpi_srcu_cb *cb = container_of(work, struct lkpi_srcu_cb, work);

	synchronize_srcu(cb->ssp);
	cb->func(cb->head);
	kfree(cb);
	if (atomic_dec_and_test(&lkpi_srcu_inflight))
		wake_up_all(&lkpi_srcu_wq);
}

void lkpi_call_srcu(struct srcu_struct *ssp, struct rcu_head *head, rcu_callback_t func)
{
	struct lkpi_srcu_cb *cb = kmalloc(sizeof(*cb), GFP_ATOMIC);

	if (WARN_ON(!cb)) {
		synchronize_srcu(ssp);
		func(head);
		return;
	}
	cb->ssp = ssp;
	cb->head = head;
	cb->func = func;
	INIT_WORK(&cb->work, lkpi_srcu_cb_fn);
	atomic_inc(&lkpi_srcu_inflight);
	queue_work(system_unbound_wq, &cb->work);
}
EXPORT_SYMBOL(lkpi_call_srcu);

void lkpi_srcu_barrier(struct srcu_struct *ssp)
{
	(void)ssp;
	wait_event(lkpi_srcu_wq, !atomic_read(&lkpi_srcu_inflight));
}
EXPORT_SYMBOL(lkpi_srcu_barrier);

void lkpi_cleanup_srcu_struct(struct srcu_struct *ssp)
{
	lkpi_srcu_barrier(ssp);
	WARN_ON(!lkpi_srcu_idx_idle(ssp, 0) || !lkpi_srcu_idx_idle(ssp, 1));
}
EXPORT_SYMBOL(lkpi_cleanup_srcu_struct);

/* -------------------------------------------------------- init / exit */

int lkpi_rcu_init(void)
{
	lkpi_rcu_task = lkpi_kthread_create_internal(lkpi_rcu_thread, NULL, "lkpi_rcu");
	return IS_ERR(lkpi_rcu_task) ? PTR_ERR(lkpi_rcu_task) : 0;
}

void lkpi_rcu_exit(void)
{
	rcu_barrier();
	kthread_stop(lkpi_rcu_task);
	lkpi_rcu_task = NULL;
}
