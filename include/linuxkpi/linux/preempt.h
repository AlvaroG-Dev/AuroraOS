/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Expulsion y contexto. preempt_disable() es el de Aurora (contador por
 * tarea: el tick no cambia de tarea mientras sea > 0). Los timers, tasklets
 * e irq_work de la capa corren en un hilo (lkpi_softirqd), que cuenta como
 * "softirq"; la capa no sabe todavia si esta dentro de una IRQ de Aurora
 * (in_hardirq() es 0 hasta L3).
 */
#ifndef _LKPI_LINUX_PREEMPT_H
#define _LKPI_LINUX_PREEMPT_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/irqflags.h>

void lkpi_preempt_disable(void);
void lkpi_preempt_enable(void);
int lkpi_preempt_count(void);
bool lkpi_in_serving_softirq(void);

#define preempt_disable() lkpi_preempt_disable()
#define preempt_enable() lkpi_preempt_enable()
#define preempt_enable_no_resched() lkpi_preempt_enable()
#define preempt_disable_notrace() lkpi_preempt_disable()
#define preempt_enable_notrace() lkpi_preempt_enable()
#define preempt_check_resched() do { } while (0)
#define preempt_count() lkpi_preempt_count()

#define in_nmi() 0
#define in_hardirq() 0
#define in_irq() in_hardirq()
#define in_serving_softirq() lkpi_in_serving_softirq()
#define in_softirq() in_serving_softirq()
#define in_interrupt() (in_hardirq() || in_softirq())
#define in_task() (!in_interrupt())
#define irq_count() (in_interrupt() ? 1 : 0)
#define in_atomic() (preempt_count() != 0)
#define preemptible() (preempt_count() == 0 && !irqs_disabled())

#define migrate_disable() preempt_disable()
#define migrate_enable() preempt_enable()

#endif
