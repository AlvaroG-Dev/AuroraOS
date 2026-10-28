/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * irq_work: funcion que se puede encolar desde cualquier contexto (tambien
 * con las IRQ desactivadas) y corre poco despues en lkpi_softirqd.
 */
#ifndef _LKPI_LINUX_IRQ_WORK_H
#define _LKPI_LINUX_IRQ_WORK_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/llist.h>

struct irq_work {
	union {
		struct llist_node llnode;
		struct { struct llist_node llist; } node;	/* work->node.llist, como Linux */
	};
	atomic_t flags;
	void (*func)(struct irq_work *);
};

#define IRQ_WORK_PENDING 0x01
#define IRQ_WORK_BUSY 0x02
#define IRQ_WORK_LAZY 0x04
#define IRQ_WORK_HARD_IRQ 0x08
#define IRQ_WORK_CLAIMED (IRQ_WORK_PENDING | IRQ_WORK_BUSY)

#define IRQ_WORK_INIT(_func) { .flags = ATOMIC_INIT(0), .func = (_func) }
#define IRQ_WORK_INIT_LAZY(_func) { .flags = ATOMIC_INIT(IRQ_WORK_LAZY), .func = (_func) }
#define IRQ_WORK_INIT_HARD(_func) { .flags = ATOMIC_INIT(IRQ_WORK_HARD_IRQ), .func = (_func) }
#define DEFINE_IRQ_WORK(name, _f) struct irq_work name = IRQ_WORK_INIT(_f)

static inline void init_irq_work(struct irq_work *work, void (*func)(struct irq_work *))
{
	atomic_set(&work->flags, 0);
	work->func = func;
}
static inline bool irq_work_is_pending(struct irq_work *work)
{
	return atomic_read(&work->flags) & IRQ_WORK_PENDING;
}
static inline bool irq_work_is_busy(struct irq_work *work)
{
	return atomic_read(&work->flags) & IRQ_WORK_BUSY;
}

bool lkpi_irq_work_queue(struct irq_work *work);
void lkpi_irq_work_sync(struct irq_work *work);
#define irq_work_queue(w) lkpi_irq_work_queue(w)
#define irq_work_queue_on(w, cpu) lkpi_irq_work_queue(w)
#define irq_work_sync(w) lkpi_irq_work_sync(w)

#endif
