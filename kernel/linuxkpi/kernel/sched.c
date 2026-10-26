// SPDX-License-Identifier: GPL-2.0-only
/*
 * task_struct de la capa, schedule()/wake_up_process(), kthreads y CPUs.
 *
 * Cada tarea de Aurora que entra en la capa tiene un task_struct, buscado
 * por su task_t en una tabla hash que se recorre sin cerrojo. Las entradas
 * no se liberan hasta descargar la capa: la de un kthread terminado queda
 * libre (__lkpi_atask == NULL, sin referencias) y se reutiliza en la misma
 * cadena. Las de tareas ajenas (p. ej. el hilo de pruebas) viven hasta la
 * descarga; si Aurora reutiliza la direccion de un task_t muerto, la tarea
 * nueva hereda ese descriptor (sin consecuencias en K2: pid y comm son
 * informativos).
 *
 * Dormir: cada task_struct lleva una cola de espera de Aurora propia. Ver
 * linux/sched.h.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/hash.h>
#include <linux/kthread.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/sprintf.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

_Static_assert(sizeof(((struct task_struct *)0)->__lkpi_park) == sizeof(struct aurora_wait_queue),
	       "__lkpi_park no cabe una wait_queue_t de Aurora");

#define LKPI_TASK_HASH_BITS 8
static struct task_struct *lkpi_task_hash[1 << LKPI_TASK_HASH_BITS];
static DEFINE_RAW_SPINLOCK(lkpi_task_lock);
static atomic_t lkpi_next_pid = ATOMIC_INIT(0);

static inline struct aurora_wait_queue *lkpi_park(struct task_struct *p)
{
	return (struct aurora_wait_queue *)p->__lkpi_park;
}

static inline unsigned int lkpi_task_bucket(struct aurora_task *at)
{
	return hash_ptr(at, LKPI_TASK_HASH_BITS);
}

static struct task_struct *lkpi_task_lookup(struct aurora_task *at)
{
	struct task_struct *p;

	for (p = READ_ONCE(lkpi_task_hash[lkpi_task_bucket(at)]); p;
	     p = READ_ONCE(p->__lkpi_hnext))
		if (READ_ONCE(p->__lkpi_atask) == at)
			return p;
	return NULL;
}

/*
 * Publica un task_struct para `at`: reutiliza una entrada libre de su
 * cadena o enlaza `spare` (que el llamante reservo fuera del cerrojo).
 * Devuelve la entrada usada; *used_spare dice si consumio `spare`.
 */
static struct task_struct *lkpi_task_publish(struct aurora_task *at, struct task_struct *spare,
					     const char *comm, unsigned int state,
					     unsigned int flags, bool fresh, bool *used_spare)
{
	unsigned int b = lkpi_task_bucket(at);
	struct task_struct *p;
	unsigned long irqf;

	*used_spare = false;
	raw_spin_lock_irqsave(&lkpi_task_lock, irqf);
	/*
	 * Una entrada que ya apunta a `at` es de una tarea anterior con la misma
	 * direccion de task_t si la tarea es nueva (`fresh`): se reinicia.
	 */
	p = lkpi_task_lookup(at);
	if (p && !fresh)
		goto out;
	if (p)
		goto init;
	for (p = lkpi_task_hash[b]; p; p = p->__lkpi_hnext)
		if (!p->__lkpi_atask && !atomic_read(&p->__lkpi_usage))
			break;
	if (!p) {
		p = spare;
		*used_spare = true;
		p->__lkpi_hnext = lkpi_task_hash[b];
		smp_store_release(&lkpi_task_hash[b], p);
	}
init:
	p->__state = state;
	p->flags = flags;
	p->pid = p->tgid = atomic_inc_return(&lkpi_next_pid);
	strscpy(p->comm, comm, sizeof(p->comm));
	p->mm = p->active_mm = NULL;
	atomic_set(&p->__lkpi_usage, 1);
	p->__lkpi_kthread = NULL;
	p->__lkpi_softirq = 0;
	aurora_wait_queue_init(lkpi_park(p));
	smp_store_release(&p->__lkpi_atask, at);
out:
	raw_spin_unlock_irqrestore(&lkpi_task_lock, irqf);
	return p;
}

static struct task_struct *lkpi_task_get_or_create(struct aurora_task *at, const char *comm,
						   unsigned int state, unsigned int flags, bool fresh)
{
	struct task_struct *spare, *p;
	bool used;

	spare = kzalloc(sizeof(*spare), GFP_ATOMIC);
	if (!spare)
		panic("linuxkpi: sin memoria para un task_struct");
	p = lkpi_task_publish(at, spare, comm, state, flags, fresh, &used);
	if (!used)
		kfree(spare);
	return p;
}

/* Ver lkpi_preempt_disable() en core.c: sched_current() no es atomico. */
static struct aurora_task *lkpi_aurora_current(void)
{
	struct aurora_task *at;
	unsigned long flags;

	local_irq_save(flags);
	at = aurora_sched_current();
	local_irq_restore(flags);
	return at;
}

struct task_struct *lkpi_get_current(void)
{
	struct aurora_task *at = lkpi_aurora_current();
	struct task_struct *p = lkpi_task_lookup(at);

	return p ? p : lkpi_task_get_or_create(at, "aurora", TASK_RUNNING, 0, false);
}
EXPORT_SYMBOL(lkpi_get_current);

void lkpi_put_task_struct(struct task_struct *p)
{
	/* A 0 la entrada queda libre para reutilizarla (ver arriba). */
	if (atomic_dec_and_test(&p->__lkpi_usage))
		WARN_ON(READ_ONCE(p->__lkpi_atask));
}
EXPORT_SYMBOL(lkpi_put_task_struct);

/* ----------------------------------------------------- dormir/despertar */

int lkpi_preempt_count(void)
{
	unsigned long flags;
	int n;

	local_irq_save(flags);
	n = aurora_preempt_count();
	local_irq_restore(flags);
	return n;
}
EXPORT_SYMBOL(lkpi_preempt_count);

bool lkpi_in_serving_softirq(void)
{
	struct task_struct *p = lkpi_task_lookup(lkpi_aurora_current());
	return p && p->__lkpi_softirq;
}
EXPORT_SYMBOL(lkpi_in_serving_softirq);

/*
 * Aparca la tarea actual si su __state no es TASK_RUNNING, hasta que
 * wake_up_process() la despierte o venza `deadline` (tick de Aurora; 0 =
 * sin plazo). Aurora revisa los plazos cada 8 ticks.
 */
static void lkpi_park_current(struct task_struct *p, u64 deadline)
{
	struct aurora_wait_queue *park = lkpi_park(p);
	struct aurora_task *at = p->__lkpi_atask;
	unsigned long flags;

	flags = aurora_spin_lock_irqsave(&park->lock);
	if (READ_ONCE(p->__state) == TASK_RUNNING) {
		aurora_spin_unlock_irqrestore(&park->lock, flags);
		return;
	}
	aurora_wait_queue_add_locked(park, at);
	aurora_sched_set_blocked_deadline(at, deadline);
	aurora_spin_unlock_irqrestore(&park->lock, flags);

	aurora_sched_yield();

	/* Si vencio el plazo, Aurora ya nos saco de la cola; si no, no estamos. */
	flags = aurora_spin_lock_irqsave(&park->lock);
	aurora_wait_queue_remove_locked(park, at);
	aurora_spin_unlock_irqrestore(&park->lock, flags);
}

long lkpi_schedule_timeout(long timeout)
{
	struct task_struct *p = current;
	unsigned long expire = 0;
	u64 deadline = 0;

	if (timeout < 0) {
		WARN_ONCE(1, "schedule_timeout: plazo negativo %ld\n", timeout);
		timeout = 0;
	}
	if (lkpi_preempt_count() > 0) {
		/* Dormir aqui colgaria la CPU: Aurora no cambia de tarea. */
		WARN_ONCE(1, "linuxkpi: schedule() con la expulsion desactivada\n");
		__set_current_state(TASK_RUNNING);
		cpu_relax();
		return timeout;
	}
	if (timeout != MAX_SCHEDULE_TIMEOUT) {
		if (timeout == 0) {
			__set_current_state(TASK_RUNNING);
			return 0;
		}
		expire = jiffies + timeout;
		deadline = expire;
	}
	lkpi_park_current(p, deadline);
	__set_current_state(TASK_RUNNING);
	if (timeout == MAX_SCHEDULE_TIMEOUT)
		return MAX_SCHEDULE_TIMEOUT;
	timeout = (long)(expire - jiffies);
	return timeout < 0 ? 0 : timeout;
}
EXPORT_SYMBOL(lkpi_schedule_timeout);

void lkpi_schedule(void)
{
	lkpi_schedule_timeout(MAX_SCHEDULE_TIMEOUT);
}
EXPORT_SYMBOL(lkpi_schedule);

int lkpi_wake_up_state(struct task_struct *p, unsigned int state)
{
	struct aurora_wait_queue *park = lkpi_park(p);
	unsigned long flags;

	flags = aurora_spin_lock_irqsave(&park->lock);
	if (!(READ_ONCE(p->__state) & state)) {
		aurora_spin_unlock_irqrestore(&park->lock, flags);
		return 0;
	}
	WRITE_ONCE(p->__state, TASK_RUNNING);
	aurora_wake_up_all_locked(park);
	aurora_spin_unlock_irqrestore(&park->lock, flags);
	return 1;
}
EXPORT_SYMBOL(lkpi_wake_up_state);

int lkpi_wake_up_process(struct task_struct *p)
{
	return lkpi_wake_up_state(p, TASK_NORMAL);
}
EXPORT_SYMBOL(lkpi_wake_up_process);

void lkpi_yield(void)
{
	aurora_sched_yield();
}
EXPORT_SYMBOL(lkpi_yield);

/* ------------------------------------------------------------ kthreads */

struct lkpi_kthread {
	struct aurora_kthread *akt;
	struct task_struct *task;
	int (*fn)(void *data);
	void *data;
	int result;
	bool should_stop;
};

static int lkpi_kthread_main(void *arg)
{
	struct lkpi_kthread *kt = arg;
	struct task_struct *p;
	int ret;

	/* El creador publica el task_struct antes de arrancar el hilo. */
	p = smp_load_acquire(&kt->task);
	/*
	 * Como en Linux: dormido hasta el primer wake_up_process(). El creador
	 * deja __state en TASK_UNINTERRUPTIBLE; aqui no se toca, para no pisar
	 * un despertar que llegue antes de que el hilo corra.
	 */
	while (!READ_ONCE(kt->should_stop) && READ_ONCE(p->__state) != TASK_RUNNING)
		schedule();
	ret = READ_ONCE(kt->should_stop) ? -EINTR : kt->fn(kt->data);
	kt->result = ret;
	/* Ya no hay `current` para este hilo: la entrada se puede reutilizar. */
	smp_store_release(&p->__lkpi_atask, NULL);
	return ret;
}

static struct task_struct *lkpi_kthread_create_v(int (*fn)(void *), void *data,
						 const char *namefmt, va_list ap)
{
	char comm[TASK_COMM_LEN];
	struct lkpi_kthread *kt;
	struct task_struct *p;

	vsnprintf(comm, sizeof(comm), namefmt, ap);
	kt = kzalloc(sizeof(*kt), GFP_KERNEL);
	if (!kt)
		return ERR_PTR(-ENOMEM);
	kt->fn = fn;
	kt->data = data;
	kt->akt = aurora_kthread_create(lkpi_kthread_main, kt, comm);
	if (!kt->akt) {
		kfree(kt);
		return ERR_PTR(-ENOMEM);
	}
	p = lkpi_task_get_or_create(aurora_kthread_task(kt->akt), comm, TASK_UNINTERRUPTIBLE,
				    PF_KTHREAD, true);
	p->__lkpi_kthread = kt;
	smp_store_release(&kt->task, p);
	aurora_kthread_start(kt->akt);
	return p;
}

struct task_struct *lkpi_kthread_create(int (*threadfn)(void *data), void *data,
					const char *namefmt, ...)
{
	struct task_struct *p;
	va_list ap;

	va_start(ap, namefmt);
	p = lkpi_kthread_create_v(threadfn, data, namefmt, ap);
	va_end(ap);
	return p;
}
EXPORT_SYMBOL(lkpi_kthread_create);

struct task_struct *lkpi_kthread_create_internal(int (*fn)(void *), void *data, const char *name)
{
	struct task_struct *p = lkpi_kthread_create(fn, data, "%s", name);

	if (!IS_ERR(p))
		wake_up_process(p);
	return p;
}

int lkpi_kthread_stop(struct task_struct *p)
{
	struct lkpi_kthread *kt = p->__lkpi_kthread;
	int ret;

	if (WARN_ON(!kt))
		return -EINVAL;
	WRITE_ONCE(kt->should_stop, true);
	smp_mb();
	wake_up_process(p);
	aurora_kthread_stop(kt->akt);	/* espera a que el hilo termine */
	ret = kt->result;
	p->__lkpi_kthread = NULL;
	kfree(kt);
	put_task_struct(p);
	return ret;
}
EXPORT_SYMBOL(lkpi_kthread_stop);

bool lkpi_kthread_should_stop(void)
{
	struct lkpi_kthread *kt = current->__lkpi_kthread;
	return kt && READ_ONCE(kt->should_stop);
}
EXPORT_SYMBOL(lkpi_kthread_should_stop);

void *lkpi_kthread_data(struct task_struct *p)
{
	struct lkpi_kthread *kt = p->__lkpi_kthread;
	return kt ? kt->data : NULL;
}
EXPORT_SYMBOL(lkpi_kthread_data);

/* ---------------------------------------------------------------- CPUs */

struct cpumask lkpi_cpu_online_mask;
EXPORT_SYMBOL(lkpi_cpu_online_mask);
unsigned int lkpi_nr_cpu_ids = 1;
EXPORT_SYMBOL(lkpi_nr_cpu_ids);
static struct cpumask lkpi_cpu_bit_masks[NR_CPUS];

const struct cpumask *__lkpi_cpumask_of(unsigned int cpu)
{
	return &lkpi_cpu_bit_masks[cpu < NR_CPUS ? cpu : 0];
}
EXPORT_SYMBOL(__lkpi_cpumask_of);

void __percpu *lkpi_alloc_percpu(size_t size, size_t align, gfp_t gfp)
{
	(void)align;	/* sizeof(tipo) ya es multiplo de su alineacion */
	return kzalloc(size * NR_CPUS, gfp);
}
EXPORT_SYMBOL(lkpi_alloc_percpu);

void lkpi_free_percpu(void __percpu *p)
{
	kfree(p);
}
EXPORT_SYMBOL(lkpi_free_percpu);

int lkpi_sched_init(void)
{
	unsigned int cpu;

	/*
	 * En linea = CPUs que ya han tenido un tick quiescente (todas las que
	 * arrancaron: la capa se carga despues del arranque SMP).
	 */
	cpumask_clear(&lkpi_cpu_online_mask);
	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		cpumask_clear(&lkpi_cpu_bit_masks[cpu]);
		cpumask_set_cpu(cpu, &lkpi_cpu_bit_masks[cpu]);
		if (aurora_sched_rcu_qs_count(cpu)) {
			cpumask_set_cpu(cpu, &lkpi_cpu_online_mask);
			lkpi_nr_cpu_ids = cpu + 1;
		}
	}
	cpumask_set_cpu(raw_smp_processor_id(), &lkpi_cpu_online_mask);
	if (num_online_cpus() != (unsigned int)aurora_smp_online_cpus())
		pr_warn("linuxkpi: %u CPUs con ticks, Aurora dice %d en linea\n",
			num_online_cpus(), aurora_smp_online_cpus());
	pr_info("linuxkpi: %u CPUs en linea\n", num_online_cpus());
	return 0;
}

void lkpi_sched_exit(void)
{
	unsigned int b;

	for (b = 0; b < ARRAY_SIZE(lkpi_task_hash); b++) {
		struct task_struct *p = lkpi_task_hash[b], *n;

		lkpi_task_hash[b] = NULL;
		for (; p; p = n) {
			n = p->__lkpi_hnext;
			kfree(p);
		}
	}
}
