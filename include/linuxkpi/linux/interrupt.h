/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * De momento solo tasklets (K2). Corren en el hilo lkpi_softirqd; un
 * tasklet no se ejecuta en dos CPU a la vez. request_irq() y compania
 * llegan con K5 (L3).
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
