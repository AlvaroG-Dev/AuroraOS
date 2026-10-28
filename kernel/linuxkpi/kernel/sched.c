// SPDX-License-Identifier: GPL-2.0-only
/*
 * task_struct de la capa, schedule()/wake_up_process(), kthreads y CPUs.
 *
 * Cada tarea de Aurora que entra en la capa tiene un task_struct, buscado
 * por su task_t en una tabla hash que se recorre sin cerrojo. Las entradas
 * no se liberan hasta descargar la capa: la de un kthread terminado queda
 * libre (__lkpi_atask == NULL, sin referencias) y se reutiliza en la misma
 * cadena.
 *
 * K8 (L4): la entrada guarda el tid de Aurora. Si Aurora reutiliza la
 * direccion de un task_t muerto, `current` lo nota (otro tid) y refresca la
 * entrada (pid, comm, mm, credenciales); si un driver aun tiene una
 * referencia a la vieja (get_task_struct), la deja y publica otra. Igual con
 * current->mm: un mm_struct por process_t, validado por pid.
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
#include <linux/sched/mm.h>
#include <linux/capability.h>
#include <linux/rwsem.h>
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

static struct mm_struct *lkpi_mm_lookup(struct aurora_process *proc, u32 pid);

/* pid, comm, credenciales y mm de Aurora. Con lkpi_task_lock tomado. */
static void lkpi_task_set_identity(struct task_struct *p, struct aurora_task *at,
				   const char *comm)
{
	struct aurora_task_info ti;

	if (aurora_task_info_get(at, &ti) != 0) {
		memset(&ti, 0, sizeof(ti));
		ti.tid = ti.tgid = atomic_inc_return(&lkpi_next_pid);
		ti.is_kernel = 1;
	}
	p->__lkpi_tid = ti.tid;
	p->pid = ti.tid;
	p->group_leader = p;
	p->exit_code = 0;
	p->tgid = ti.tgid;
	p->__lkpi_uid = ti.uid;
	p->__lkpi_euid = ti.euid;
	/* Los kthreads de la capa llevan su nombre de Linux; el resto, el de Aurora. */
	strscpy(p->comm, comm ? comm : ti.comm, sizeof(p->comm));
	p->mm = p->active_mm = ti.proc ? lkpi_mm_lookup(ti.proc, ti.tgid) : NULL;
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
	if (p && atomic_read(&p->__lkpi_usage) <= 1)
		goto init;
	if (p) {
		/* Alguien guarda la vieja: queda fuera de la busqueda y se libera
		 * para reutilizarla cuando suelte su referencia. */
		smp_store_release(&p->__lkpi_atask, NULL);
		if (!p->__lkpi_kthread)
			atomic_dec(&p->__lkpi_usage);	/* la de la tabla */
	}
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
	lkpi_task_set_identity(p, at, comm);
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

static inline struct aurora_task *lkpi_aurora_current(void)
{
	return aurora_sched_current();	/* lectura atomica de %gs:current_task */
}

struct task_struct *lkpi_get_current(void)
{
	struct aurora_task *at = lkpi_aurora_current();
	struct task_struct *p = lkpi_task_lookup(at);

	if (likely(p) && likely(READ_ONCE(p->__lkpi_tid) == aurora_task_tid(at)))
		return p;
	/* Nueva, o una tarea nueva con la direccion de un task_t muerto. */
	return lkpi_task_get_or_create(at, NULL, TASK_RUNNING, 0, !!p);
}
EXPORT_SYMBOL(lkpi_get_current);

void lkpi_put_task_struct(struct task_struct *p)
{
	/* A 0 la entrada queda libre para reutilizarla (ver arriba). */
	if (atomic_dec_and_test(&p->__lkpi_usage))
		WARN_ON(READ_ONCE(p->__lkpi_atask));
}
EXPORT_SYMBOL(lkpi_put_task_struct);

/* ------------------------------------------------------- mm y senales */

#define LKPI_MM_HASH_BITS 6
static struct mm_struct *lkpi_mm_hash[1 << LKPI_MM_HASH_BITS];

/*
 * mm_struct del proceso `proc` (pid `pid`). Con lkpi_task_lock tomado. Si la
 * entrada es de un proceso muerto con la misma direccion de process_t, se
 * reinicia; si alguien la guarda, se deja y se crea otra.
 */
static struct mm_struct *lkpi_mm_lookup(struct aurora_process *proc, u32 pid)
{
	unsigned int b = hash_ptr(proc, LKPI_MM_HASH_BITS);
	struct mm_struct *mm, *freeslot = NULL;

	for (mm = lkpi_mm_hash[b]; mm; mm = mm->__lkpi_hnext) {
		if (mm->__lkpi_proc == proc && mm->__lkpi_pid == pid)
			return mm;
		if (mm->__lkpi_proc == proc)	/* muerto: ya no se encuentra */
			mm->__lkpi_proc = NULL;
		if (!mm->__lkpi_proc && atomic_read(&mm->mm_count) <= 1 &&
		    atomic_read(&mm->mm_users) <= 1 && !freeslot)
			freeslot = mm;
	}
	mm = freeslot;
	if (!mm) {
		mm = kzalloc(sizeof(*mm), GFP_ATOMIC);
		if (!mm)
			return NULL;
		mm->__lkpi_hnext = lkpi_mm_hash[b];
		lkpi_mm_hash[b] = mm;
	}
	atomic_set(&mm->mm_users, 1);	/* las de la tabla */
	atomic_set(&mm->mm_count, 1);
	init_rwsem(&mm->mmap_lock);
	mm->task_size = 0x00007FFFFFFFF000UL;
	mm->__lkpi_pid = pid;
	mm->__lkpi_proc = proc;
	return mm;
}

/* Las entradas de mm viven hasta descargar la capa (como las de tareas). */
void lkpi_mmput(struct mm_struct *mm)
{
	WARN_ON(atomic_dec_return(&mm->mm_users) < 1);
}
EXPORT_SYMBOL(lkpi_mmput);

void lkpi_mmdrop(struct mm_struct *mm)
{
	WARN_ON(atomic_dec_return(&mm->mm_count) < 1);
}
EXPORT_SYMBOL(lkpi_mmdrop);

struct mm_struct *lkpi_get_task_mm(struct task_struct *p)
{
	struct mm_struct *mm = READ_ONCE(p->mm);

	if (mm)
		atomic_inc(&mm->mm_users);
	return mm;
}
EXPORT_SYMBOL(lkpi_get_task_mm);

int lkpi_signal_pending(struct task_struct *p)
{
	struct aurora_task *at = READ_ONCE(p->__lkpi_atask);

	return at && aurora_signal_unblocked_of(at) != 0;
}
EXPORT_SYMBOL(lkpi_signal_pending);

int lkpi_fatal_signal_pending(struct task_struct *p)
{
	struct aurora_task *at = READ_ONCE(p->__lkpi_atask);

	return at && (aurora_signal_unblocked_of(at) & (1ULL << AURORA_SIGKILL));
}
EXPORT_SYMBOL(lkpi_fatal_signal_pending);

bool lkpi_capable(int cap)
{
	struct task_struct *p = current;

	(void)cap;
	return !p->mm || p->__lkpi_euid == 0;	/* hilos del kernel o root */
}
EXPORT_SYMBOL(lkpi_capable);

/* ----------------------------------------------------- dormir/despertar */

int lkpi_preempt_count(void)
{
	/* Como Linux: dentro de un handler de request_irq() suma HARDIRQ_OFFSET. */
	return aurora_preempt_count() + lkpi_hardirq_count() * HARDIRQ_OFFSET;
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
 * sin plazo). Aurora despierta en el tick en que vence el plazo.
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
	/* NR_CPUS copias: puede ser grande, no hace falta que sea contigua. */
	return kvzalloc(size * NR_CPUS, gfp);
}
EXPORT_SYMBOL(lkpi_alloc_percpu);

void lkpi_free_percpu(void __percpu *p)
{
	kvfree(p);
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

	for (b = 0; b < ARRAY_SIZE(lkpi_mm_hash); b++) {
		struct mm_struct *mm = lkpi_mm_hash[b], *n;

		lkpi_mm_hash[b] = NULL;
		for (; mm; mm = n) {
			n = mm->__lkpi_hnext;
			kfree(mm);
		}
	}

	for (b = 0; b < ARRAY_SIZE(lkpi_task_hash); b++) {
		struct task_struct *p = lkpi_task_hash[b], *n;

		lkpi_task_hash[b] = NULL;
		for (; p; p = n) {
			n = p->__lkpi_hnext;
			kfree(p);
		}
	}
}
