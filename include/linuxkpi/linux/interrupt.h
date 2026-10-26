/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Interrupciones (K5) y tasklets (K2).
 *
 * request_irq(): el handler corre en la IRQ real de Aurora (interrupciones
 * desactivadas, in_hardirq() cierto). El numero de IRQ es el de Aurora:
 * 0..15 para INTx (IOAPIC) y el vector (0x60..0x7F) para MSI/MSI-X; lo da
 * pci_irq_vector(). La parte con hilo (request_threaded_irq, IRQ_WAKE_THREAD,
 * IRQF_ONESHOT) corre en un kthread de la capa por accion ("irq/N-nombre").
 * IRQF_SHARED vale entre drivers de Linux; una linea que ya usa un driver de
 * Aurora da -EBUSY.
 *
 * Los tasklets corren en el hilo lkpi_softirqd; un tasklet no se ejecuta en
 * dos CPU a la vez.
 */
#ifndef _LKPI_LINUX_INTERRUPT_H
#define _LKPI_LINUX_INTERRUPT_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/preempt.h>
#include <linux/bottom_half.h>
#include <linux/irqflags.h>
#include <linux/container_of.h>
#include <linux/irqreturn.h>
#include <linux/cpumask.h>
#include <linux/errno.h>

/* ---------------------------------------------------------------- IRQ */

typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

#define IRQF_TRIGGER_NONE 0x00000000
#define IRQF_TRIGGER_RISING 0x00000001
#define IRQF_TRIGGER_FALLING 0x00000002
#define IRQF_TRIGGER_HIGH 0x00000004
#define IRQF_TRIGGER_LOW 0x00000008
#define IRQF_TRIGGER_MASK (IRQF_TRIGGER_HIGH | IRQF_TRIGGER_LOW | \
			   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)
#define IRQF_TRIGGER_PROBE 0x00000010
#define IRQF_SHARED 0x00000080
#define IRQF_PROBE_SHARED 0x00000100
#define __IRQF_TIMER 0x00000200
#define IRQF_PERCPU 0x00000400
#define IRQF_NOBALANCING 0x00000800
#define IRQF_IRQPOLL 0x00001000
#define IRQF_ONESHOT 0x00002000
#define IRQF_NO_SUSPEND 0x00004000
#define IRQF_FORCE_RESUME 0x00008000
#define IRQF_NO_THREAD 0x00010000
#define IRQF_EARLY_RESUME 0x00020000
#define IRQF_COND_SUSPEND 0x00040000
#define IRQF_NO_AUTOEN 0x00080000
#define IRQF_NO_DEBUG 0x00100000
#define IRQF_TIMER (__IRQF_TIMER | IRQF_NO_SUSPEND | IRQF_NO_THREAD)

enum {
	IRQC_IS_HARDIRQ = 0,
	IRQC_IS_NESTED,
};

int lkpi_request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
			      unsigned long flags, const char *name, void *dev);
const void *lkpi_free_irq(unsigned int irq, void *dev_id);
void lkpi_disable_irq(unsigned int irq);
void lkpi_disable_irq_nosync(unsigned int irq);
bool lkpi_disable_hardirq(unsigned int irq);
void lkpi_enable_irq(unsigned int irq);
void lkpi_synchronize_irq(unsigned int irq);
bool lkpi_synchronize_hardirq(unsigned int irq);
int lkpi_irq_set_affinity(unsigned int irq, const struct cpumask *cpumask);
const struct cpumask *lkpi_irq_get_affinity_mask(int irq);
int lkpi_irq_get_effective_cpu(unsigned int irq);
bool lkpi_irq_has_action(unsigned int irq);

struct device;
int lkpi_devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
				   irq_handler_t thread_fn, unsigned long irqflags,
				   const char *devname, void *dev_id);
void lkpi_devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);

#define request_threaded_irq(i, h, t, f, n, d) lkpi_request_threaded_irq(i, h, t, f, n, d)
#define free_irq(i, d) lkpi_free_irq(i, d)
#define disable_irq(i) lkpi_disable_irq(i)
#define disable_irq_nosync(i) lkpi_disable_irq_nosync(i)
#define disable_hardirq(i) lkpi_disable_hardirq(i)
#define enable_irq(i) lkpi_enable_irq(i)
#define synchronize_irq(i) lkpi_synchronize_irq(i)
#define synchronize_hardirq(i) lkpi_synchronize_hardirq(i)
#define irq_set_affinity(i, m) lkpi_irq_set_affinity(i, m)
#define irq_force_affinity(i, m) lkpi_irq_set_affinity(i, m)
#define irq_get_affinity_mask(i) lkpi_irq_get_affinity_mask(i)
#define irq_has_action(i) lkpi_irq_has_action(i)
#define devm_request_threaded_irq(d, i, h, t, f, n, x) \
	lkpi_devm_request_threaded_irq(d, i, h, t, f, n, x)
#define devm_free_irq(d, i, x) lkpi_devm_free_irq(d, i, x)

static inline int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
			      const char *name, void *dev)
{
	return request_threaded_irq(irq, handler, NULL, flags, name, dev);
}
static inline int request_any_context_irq(unsigned int irq, irq_handler_t handler,
					  unsigned long flags, const char *name, void *dev_id)
{
	int ret = request_irq(irq, handler, flags, name, dev_id);
	return ret ? ret : IRQC_IS_HARDIRQ;
}
static inline int devm_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
				   unsigned long irqflags, const char *devname, void *dev_id)
{
	return devm_request_threaded_irq(dev, irq, handler, NULL, irqflags, devname, dev_id);
}
/* La afinidad pedida es la efectiva: no hay "pista" aparte. */
static inline int irq_update_affinity_hint(unsigned int irq, const struct cpumask *m)
{
	return 0;
}
static inline int irq_set_affinity_and_hint(unsigned int irq, const struct cpumask *m)
{
	return m ? irq_set_affinity(irq, m) : 0;
}
static inline int irq_set_affinity_hint(unsigned int irq, const struct cpumask *m)
{
	return irq_set_affinity_and_hint(irq, m);
}
/* Sin suspension (L9): ninguna IRQ despierta al sistema. */
static inline int enable_irq_wake(unsigned int irq) { return 0; }
static inline int disable_irq_wake(unsigned int irq) { return 0; }
static inline int irq_set_irq_wake(unsigned int irq, unsigned int on) { return 0; }

/* Reparto de vectores entre CPU (pci_alloc_irq_vectors_affinity). */
#define IRQ_AFFINITY_MAX_SETS 4
struct irq_affinity {
	unsigned int pre_vectors;
	unsigned int post_vectors;
	unsigned int nr_sets;
	unsigned int set_size[IRQ_AFFINITY_MAX_SETS];
	void (*calc_sets)(struct irq_affinity *, unsigned int nvecs);
	void *priv;
};
struct irq_affinity_desc {
	struct cpumask mask;
	unsigned int is_managed : 1;
};

struct tasklet_struct {
	struct tasklet_struct *next;
	unsigned long state;
	atomic_t count;			/* > 0: desactivado */
	bool use_callback;
	union {
		void (*func)(unsigned long data);
		void (*callback)(struct tasklet_struct *t);
	};
	unsigned long data;
};

enum {
	TASKLET_STATE_SCHED,	/* encolado */
	TASKLET_STATE_RUN,	/* ejecutandose */
};

#define DECLARE_TASKLET(name, _callback) \
	struct tasklet_struct name = { .count = ATOMIC_INIT(0), .use_callback = true, \
				       .callback = _callback }
#define DECLARE_TASKLET_DISABLED(name, _callback) \
	struct tasklet_struct name = { .count = ATOMIC_INIT(1), .use_callback = true, \
				       .callback = _callback }
#define DECLARE_TASKLET_OLD(name, _func) \
	struct tasklet_struct name = { .count = ATOMIC_INIT(0), .func = _func }
#define from_tasklet(var, callback_tasklet, tasklet_fieldname) \
	container_of(callback_tasklet, typeof(*var), tasklet_fieldname)

void lkpi_tasklet_schedule(struct tasklet_struct *t);
void lkpi_tasklet_kill(struct tasklet_struct *t);
void lkpi_tasklet_unlock_wait(struct tasklet_struct *t);
void lkpi_tasklet_enable(struct tasklet_struct *t);

static inline void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
				unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->use_callback = false;
	t->func = func;
	t->data = data;
}
static inline void tasklet_setup(struct tasklet_struct *t,
				 void (*callback)(struct tasklet_struct *))
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->use_callback = true;
	t->callback = callback;
	t->data = 0;
}
#define tasklet_schedule(t) lkpi_tasklet_schedule(t)
#define tasklet_hi_schedule(t) lkpi_tasklet_schedule(t)
#define tasklet_kill(t) lkpi_tasklet_kill(t)
#define tasklet_unlock_wait(t) lkpi_tasklet_unlock_wait(t)
#define tasklet_unlock_spin_wait(t) lkpi_tasklet_unlock_wait(t)
#define tasklet_enable(t) lkpi_tasklet_enable(t)
static inline bool tasklet_trylock(struct tasklet_struct *t)
{
	return !test_and_set_bit(TASKLET_STATE_RUN, &t->state);
}
static inline void tasklet_unlock(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	clear_bit(TASKLET_STATE_RUN, &t->state);
}
static inline void tasklet_disable_nosync(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
	smp_mb__after_atomic();
}
static inline void tasklet_disable(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_wait(t);
	smp_mb();
}
#define tasklet_disable_in_atomic(t) tasklet_disable(t)
static inline bool tasklet_is_scheduled(struct tasklet_struct *t)
{
	return test_bit(TASKLET_STATE_SCHED, &t->state);
}

#endif
