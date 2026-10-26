// SPDX-License-Identifier: GPL-2.0-only
/*
 * Interrupciones de Linux sobre las de Aurora (K5).
 *
 * Cada IRQ de la capa (descriptor lkpi_irq_desc) la crea quien conoce el
 * hardware: pci/msi.c para INTx, MSI y MSI-X. El numero es el de Aurora
 * (0..15 INTx, 0x60..0x7F vectores MSI). Con la primera accion se registra
 * en Aurora (irq_request_threaded) una funcion de entrada, que corre en la
 * IRQ real con las interrupciones desactivadas, cuenta in_hardirq() y llama
 * a los handlers de Linux. La parte con hilo va en un kthread de la capa
 * por accion. El "chip" (mascara, afinidad) lo aporta pci/msi.c.
 *
 * Como Linux: disable_irq() cuenta anidamientos; una IRQ flanco (MSI) que
 * llega deshabilitada queda pendiente y se reproduce en enable_irq().
 * IRQF_ONESHOT enmascara una linea de nivel (INTx) hasta que termina el hilo.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/device.h>
#include "../aurora.h"
#include "../lkpi_internal.h"

#define LKPI_NR_IRQS (AURORA_MSI_VECTOR_END + 1)

struct lkpi_irqaction {
	struct lkpi_irqaction *next;
	irq_handler_t handler;
	irq_handler_t thread_fn;
	void *dev_id;
	unsigned long flags;
	const char *name;
	struct lkpi_irq_desc *desc;
	struct task_struct *thread;
	unsigned long thread_flags;	/* bit 0: hay que correr thread_fn */
};

#define LKPI_IRQTF_RUNTHREAD 0

static struct lkpi_irq_desc *lkpi_irq_descs[LKPI_NR_IRQS];
static DEFINE_SPINLOCK(lkpi_irq_table_lock);

/* in_hardirq(): profundidad por CPU. Solo cambia con las IRQ desactivadas. */
static unsigned int lkpi_hardirq_cnt[NR_CPUS];

unsigned int lkpi_hardirq_count(void)
{
	unsigned long flags;
	unsigned int n;

	/* Con las IRQ desactivadas la tarea no puede cambiar de CPU entre leer
	 * el numero de CPU y el contador. */
	local_irq_save(flags);
	n = lkpi_hardirq_cnt[raw_smp_processor_id()];
	local_irq_restore(flags);
	return n;
}
EXPORT_SYMBOL(lkpi_hardirq_count);

static struct lkpi_irq_desc *lkpi_irq_to_desc(unsigned int irq)
{
	struct lkpi_irq_desc *d;

	if (irq >= LKPI_NR_IRQS)
		return NULL;
	spin_lock(&lkpi_irq_table_lock);
	d = lkpi_irq_descs[irq];
	spin_unlock(&lkpi_irq_table_lock);
	return d;
}

/* --------------------------------------------- descriptores (msi.c) */

struct lkpi_irq_desc *lkpi_irq_desc_alloc(unsigned int irq, const struct lkpi_irq_chip *chip,
					  void *chip_data, int cpu, bool level)
{
	struct lkpi_irq_desc *d;

	if (irq >= LKPI_NR_IRQS)
		return ERR_PTR(-EINVAL);
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return ERR_PTR(-ENOMEM);
	d->irq = irq;
	d->chip = chip;
	d->chip_data = chip_data;
	d->cpu = cpu;
	d->level = level;
	d->masked = true;		/* el chip lo deja enmascarado hasta startup */
	d->users = 1;
	cpumask_copy(&d->affinity, cpumask_of(cpu));
	raw_spin_lock_init(&d->lock);
	mutex_init(&d->request_mutex);
	init_waitqueue_head(&d->wait_for_threads);
	snprintf(d->name, sizeof(d->name), "lkpi-%u", irq);
	spin_lock(&lkpi_irq_table_lock);
	if (lkpi_irq_descs[irq]) {
		spin_unlock(&lkpi_irq_table_lock);
		kfree(d);
		return ERR_PTR(-EBUSY);
	}
	lkpi_irq_descs[irq] = d;
	spin_unlock(&lkpi_irq_table_lock);
	return d;
}

/* Una linea INTx que ya usa otro dispositivo de la capa: compartirla. */
struct lkpi_irq_desc *lkpi_irq_desc_get_shared(unsigned int irq)
{
	struct lkpi_irq_desc *d;

	if (irq >= LKPI_NR_IRQS)
		return NULL;
	spin_lock(&lkpi_irq_table_lock);
	d = lkpi_irq_descs[irq];
	if (d && d->level)
		d->users++;
	else
		d = NULL;
	spin_unlock(&lkpi_irq_table_lock);
	return d;
}

void lkpi_irq_desc_free(struct lkpi_irq_desc *d)
{
	bool last;

	if (!d)
		return;
	spin_lock(&lkpi_irq_table_lock);
	last = --d->users == 0;
	spin_unlock(&lkpi_irq_table_lock);
	if (!last)
		return;
	while (d->action) {
		WARN(1, "IRQ %u liberada con un handler ('%s') todavia instalado\n", d->irq,
		     d->action->name);
		lkpi_free_irq(d->irq, d->action->dev_id);
	}
	spin_lock(&lkpi_irq_table_lock);
	if (lkpi_irq_descs[d->irq] == d)
		lkpi_irq_descs[d->irq] = NULL;
	spin_unlock(&lkpi_irq_table_lock);
	kfree(d);
}

static void lkpi_irq_mask(struct lkpi_irq_desc *d)
{
	if (!d->masked) {
		d->masked = true;
		if (d->chip->mask)
			d->chip->mask(d);
	}
}

static void lkpi_irq_unmask(struct lkpi_irq_desc *d)
{
	if (d->masked) {
		d->masked = false;
		if (d->chip->unmask)
			d->chip->unmask(d);
	}
}

/* ---------------------------------------------------------- entrada */

static void lkpi_irq_wake_thread(struct lkpi_irq_desc *d, struct lkpi_irqaction *a)
{
	if (!a->thread)
		return;
	if (test_and_set_bit(LKPI_IRQTF_RUNTHREAD, &a->thread_flags))
		return;			/* ya pendiente: una sola ejecucion */
	atomic_inc(&d->threads_active);
	if (a->flags & IRQF_ONESHOT && d->level) {
		/* La linea de nivel sigue activa hasta que el hilo atienda la causa. */
		raw_spin_lock(&d->lock);
		d->threads_oneshot++;
		lkpi_irq_mask(d);
		raw_spin_unlock(&d->lock);
	}
	wake_up_process(a->thread);
}

static void lkpi_irq_run_handlers(struct lkpi_irq_desc *d)
{
	struct lkpi_irqaction *a;

	for (a = READ_ONCE(d->action); a; a = READ_ONCE(a->next)) {
		irqreturn_t r = a->handler(d->irq, a->dev_id);

		if (r & IRQ_WAKE_THREAD)
			lkpi_irq_wake_thread(d, a);
	}
}

/* Corre en la IRQ de Aurora (IF=0, antes del EOI). */
static int lkpi_irq_entry(int irq, void *data)
{
	struct lkpi_irq_desc *d = data;
	unsigned int cpu = raw_smp_processor_id();

	lkpi_hardirq_cnt[cpu]++;
	atomic_inc(&d->in_progress);
	smp_mb__after_atomic();
	if (READ_ONCE(d->depth) > 0) {
		/* Deshabilitada: las de nivel se enmascaran (seguirian llegando);
		 * las de flanco se recuerdan para enable_irq(). */
		raw_spin_lock(&d->lock);
		if (d->depth > 0) {
			if (!d->level)
				d->pending = true;
			lkpi_irq_mask(d);
			raw_spin_unlock(&d->lock);
			goto out;
		}
		raw_spin_unlock(&d->lock);
	}
	lkpi_irq_run_handlers(d);
	d->count++;
out:
	smp_mb__before_atomic();
	atomic_dec(&d->in_progress);
	lkpi_hardirq_cnt[cpu]--;
	/* Siempre "atendida": un IRQ_NONE haria a Aurora buscar otro handler. */
	return 1;
}

/* Reproduce una IRQ de flanco pendiente, como si llegara ahora. */
static void lkpi_irq_replay(struct lkpi_irq_desc *d)
{
	unsigned long flags;

	local_irq_save(flags);
	lkpi_irq_entry(d->irq, d);
	local_irq_restore(flags);
}

/* ------------------------------------------------------------ hilos */

static void lkpi_irq_oneshot_done(struct lkpi_irq_desc *d)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&d->lock, flags);
	if (d->threads_oneshot && --d->threads_oneshot == 0 && d->depth == 0)
		lkpi_irq_unmask(d);
	raw_spin_unlock_irqrestore(&d->lock, flags);
}

static int lkpi_irq_thread(void *data)
{
	struct lkpi_irqaction *a = data;
	struct lkpi_irq_desc *d = a->desc;

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			__set_current_state(TASK_RUNNING);
			break;
		}
		if (!test_and_clear_bit(LKPI_IRQTF_RUNTHREAD, &a->thread_flags)) {
			schedule();
			continue;
		}
		__set_current_state(TASK_RUNNING);
		a->thread_fn(d->irq, a->dev_id);
		if (a->flags & IRQF_ONESHOT && d->level)
			lkpi_irq_oneshot_done(d);
		if (atomic_dec_and_test(&d->threads_active))
			wake_up(&d->wait_for_threads);
	}
	/* Pendiente sin atender al parar: no dejar la cuenta colgada. */
	if (test_and_clear_bit(LKPI_IRQTF_RUNTHREAD, &a->thread_flags)) {
		if (a->flags & IRQF_ONESHOT && d->level)
			lkpi_irq_oneshot_done(d);
		if (atomic_dec_and_test(&d->threads_active))
			wake_up(&d->wait_for_threads);
	}
	return 0;
}

/* Primario por defecto de request_threaded_irq(irq, NULL, fn, ...). */
static irqreturn_t lkpi_irq_default_primary(int irq, void *dev_id)
{
	return IRQ_WAKE_THREAD;
}

/* ------------------------------------------------------- request/free */

int lkpi_request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
			      unsigned long flags, const char *name, void *dev)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);
	struct lkpi_irqaction *a, **pp;
	unsigned long lflags;
	bool first;
	int rc;

	if (!d)
		return -EINVAL;
	if ((flags & IRQF_SHARED) && !dev)
		return -EINVAL;
	if (!handler && !thread_fn)
		return -EINVAL;
	if (!handler) {
		/* Una linea de nivel sin primario necesita ONESHOT (si no, tormenta). */
		if (d->level && !(flags & IRQF_ONESHOT)) {
			pr_err("linuxkpi: IRQ %u ('%s'): sin handler primario hace falta "
			       "IRQF_ONESHOT\n", irq, name);
			return -EINVAL;
		}
		handler = lkpi_irq_default_primary;
	}

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->handler = handler;
	a->thread_fn = thread_fn;
	a->flags = flags;
	a->name = name;
	a->dev_id = dev;
	a->desc = d;
	if (thread_fn) {
		a->thread = kthread_create(lkpi_irq_thread, a, "irq/%u-%s", irq, name ? name : "");
		if (IS_ERR(a->thread)) {
			rc = PTR_ERR(a->thread);
			kfree(a);
			return rc;
		}
		get_task_struct(a->thread);
		wake_up_process(a->thread);
	}

	mutex_lock(&d->request_mutex);
	first = !d->action;
	if (!first) {
		struct lkpi_irqaction *o = d->action;

		if (!(o->flags & flags & IRQF_SHARED) ||
		    ((o->flags ^ flags) & IRQF_ONESHOT)) {
			pr_err("linuxkpi: IRQ %u ocupada por '%s' (no compartible con '%s')\n", irq,
			       o->name, name);
			rc = -EBUSY;
			goto err_unlock;
		}
	} else {
		d->depth = (flags & IRQF_NO_AUTOEN) ? 1 : 0;
		d->pending = false;
		rc = aurora_irq_request_threaded(irq, lkpi_irq_entry, NULL, d->name, d);
		if (rc) {
			pr_err("linuxkpi: IRQ %u ('%s'): Aurora la rechaza (%d), ¿la usa un "
			       "driver propio?\n", irq, name, rc);
			goto err_unlock;
		}
	}
	raw_spin_lock_irqsave(&d->lock, lflags);
	for (pp = &d->action; *pp; pp = &(*pp)->next)
		;
	WRITE_ONCE(*pp, a);
	if (first) {
		if (d->chip->startup)
			d->chip->startup(d);
		if (d->depth == 0)
			lkpi_irq_unmask(d);
	}
	raw_spin_unlock_irqrestore(&d->lock, lflags);
	mutex_unlock(&d->request_mutex);
	return 0;

err_unlock:
	mutex_unlock(&d->request_mutex);
	if (a->thread) {
		kthread_stop(a->thread);
		put_task_struct(a->thread);
	}
	kfree(a);
	return rc;
}
EXPORT_SYMBOL(lkpi_request_threaded_irq);

static void lkpi_irq_wait_hardirq(struct lkpi_irq_desc *d)
{
	smp_mb();
	while (atomic_read(&d->in_progress))
		cpu_relax();
}

const void *lkpi_free_irq(unsigned int irq, void *dev_id)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);
	struct lkpi_irqaction *a, **pp;
	unsigned long flags;
	const char *name;
	bool last;

	if (!d)
		return NULL;
	mutex_lock(&d->request_mutex);
	raw_spin_lock_irqsave(&d->lock, flags);
	for (pp = &d->action; (a = *pp); pp = &a->next)
		if (a->dev_id == dev_id)
			break;
	if (!a) {
		raw_spin_unlock_irqrestore(&d->lock, flags);
		mutex_unlock(&d->request_mutex);
		WARN(1, "free_irq(%u): dev_id %p no registrado\n", irq, dev_id);
		return NULL;
	}
	WRITE_ONCE(*pp, a->next);
	last = !d->action;
	if (last) {
		lkpi_irq_mask(d);
		if (d->chip->shutdown)
			d->chip->shutdown(d);
	}
	raw_spin_unlock_irqrestore(&d->lock, flags);

	/* Aurora espera al ack en curso; la capa, ademas, a su propio contador. */
	if (last)
		aurora_irq_free(irq);
	lkpi_irq_wait_hardirq(d);
	if (a->thread) {
		kthread_stop(a->thread);
		put_task_struct(a->thread);
	}
	if (last) {
		d->threads_oneshot = 0;
		d->depth = 0;
		d->pending = false;
	}
	mutex_unlock(&d->request_mutex);
	name = a->name;
	kfree(a);
	return name;
}
EXPORT_SYMBOL(lkpi_free_irq);

bool lkpi_irq_has_action(unsigned int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);

	return d && READ_ONCE(d->action);
}
EXPORT_SYMBOL(lkpi_irq_has_action);

/* --------------------------------------------- disable/enable/sync */

void lkpi_disable_irq_nosync(unsigned int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);
	unsigned long flags;

	if (!d)
		return;
	raw_spin_lock_irqsave(&d->lock, flags);
	if (d->depth++ == 0)
		lkpi_irq_mask(d);
	raw_spin_unlock_irqrestore(&d->lock, flags);
}
EXPORT_SYMBOL(lkpi_disable_irq_nosync);

bool lkpi_disable_hardirq(unsigned int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);

	lkpi_disable_irq_nosync(irq);
	if (d)
		lkpi_irq_wait_hardirq(d);
	return true;
}
EXPORT_SYMBOL(lkpi_disable_hardirq);

void lkpi_disable_irq(unsigned int irq)
{
	lkpi_disable_irq_nosync(irq);
	lkpi_synchronize_irq(irq);
}
EXPORT_SYMBOL(lkpi_disable_irq);

void lkpi_enable_irq(unsigned int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);
	unsigned long flags;
	bool replay = false;

	if (!d)
		return;
	raw_spin_lock_irqsave(&d->lock, flags);
	if (d->depth == 0) {
		raw_spin_unlock_irqrestore(&d->lock, flags);
		WARN(1, "enable_irq(%u) desequilibrado\n", irq);
		return;
	}
	if (--d->depth == 0) {
		if (!d->threads_oneshot)
			lkpi_irq_unmask(d);
		replay = d->pending && d->action;
		d->pending = false;
	}
	raw_spin_unlock_irqrestore(&d->lock, flags);
	if (replay)
		lkpi_irq_replay(d);
}
EXPORT_SYMBOL(lkpi_enable_irq);

bool lkpi_synchronize_hardirq(unsigned int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);

	if (d)
		lkpi_irq_wait_hardirq(d);
	return !d || !atomic_read(&d->threads_active);
}
EXPORT_SYMBOL(lkpi_synchronize_hardirq);

void lkpi_synchronize_irq(unsigned int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);

	if (!d)
		return;
	lkpi_irq_wait_hardirq(d);
	wait_event(d->wait_for_threads, !atomic_read(&d->threads_active));
}
EXPORT_SYMBOL(lkpi_synchronize_irq);

/* ---------------------------------------------------------- afinidad */

int lkpi_irq_set_affinity(unsigned int irq, const struct cpumask *mask)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);
	struct cpumask m;
	unsigned long flags;
	int cpu, rc = 0;

	if (!d)
		return -EINVAL;
	if (!cpumask_and(&m, mask, cpu_online_mask))
		return -EINVAL;
	cpu = cpumask_first(&m);
	raw_spin_lock_irqsave(&d->lock, flags);
	if (d->chip->set_affinity)
		rc = d->chip->set_affinity(d, cpu);
	if (!rc) {
		d->cpu = cpu;
		cpumask_copy(&d->affinity, &m);
	}
	raw_spin_unlock_irqrestore(&d->lock, flags);
	return rc;
}
EXPORT_SYMBOL(lkpi_irq_set_affinity);

const struct cpumask *lkpi_irq_get_affinity_mask(int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);

	return d ? &d->affinity : NULL;
}
EXPORT_SYMBOL(lkpi_irq_get_affinity_mask);

int lkpi_irq_get_effective_cpu(unsigned int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);

	return d ? d->cpu : -EINVAL;
}
EXPORT_SYMBOL(lkpi_irq_get_effective_cpu);

/* ------------------------------------------------------------ devm */

struct lkpi_irq_devres {
	unsigned int irq;
	void *dev_id;
};

static void lkpi_devm_irq_release(struct device *dev, void *res)
{
	struct lkpi_irq_devres *r = res;

	lkpi_free_irq(r->irq, r->dev_id);
}

static int lkpi_devm_irq_match(struct device *dev, void *res, void *data)
{
	struct lkpi_irq_devres *a = res, *b = data;

	return a->irq == b->irq && a->dev_id == b->dev_id;
}

int lkpi_devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
				   irq_handler_t thread_fn, unsigned long irqflags,
				   const char *devname, void *dev_id)
{
	struct lkpi_irq_devres *r;
	int rc;

	r = devres_alloc(lkpi_devm_irq_release, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	if (!devname)
		devname = dev_name(dev);
	rc = lkpi_request_threaded_irq(irq, handler, thread_fn, irqflags, devname, dev_id);
	if (rc) {
		devres_free(r);
		return rc;
	}
	r->irq = irq;
	r->dev_id = dev_id;
	devres_add(dev, r);
	return 0;
}
EXPORT_SYMBOL(lkpi_devm_request_threaded_irq);

void lkpi_devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
	struct lkpi_irq_devres match = { irq, dev_id };

	WARN_ON(devres_release(dev, lkpi_devm_irq_release, lkpi_devm_irq_match, &match));
}
EXPORT_SYMBOL(lkpi_devm_free_irq);

/* Contador de interrupciones atendidas (para las pruebas y /proc futuro). */
u64 lkpi_irq_count(unsigned int irq)
{
	struct lkpi_irq_desc *d = lkpi_irq_to_desc(irq);

	return d ? d->count : 0;
}
EXPORT_SYMBOL(lkpi_irq_count);
