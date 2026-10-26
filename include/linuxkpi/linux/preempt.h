/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Expulsion y contexto. preempt_disable() es el de Aurora (contador por
 * tarea: el tick no cambia de tarea mientras sea > 0). Los timers, tasklets
 * e irq_work de la capa corren en un hilo (lkpi_softirqd), que cuenta como
 * "softirq". in_hardirq() es cierto mientras corre un handler de
 * request_irq() en la IRQ real (contador por CPU de la capa, L3); las IRQ
 * propias de Aurora no lo cuentan. preempt_count() suma HARDIRQ_OFFSET por
 * cada nivel, como en Linux.
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
unsigned int lkpi_hardirq_count(void);

#define PREEMPT_OFFSET (1UL << 0)
#define SOFTIRQ_OFFSET (1UL << 8)
#define HARDIRQ_OFFSET (1UL << 16)
#define NMI_OFFSET (1UL << 20)

#define preempt_disable() lkpi_preempt_disable()
#define preempt_enable() lkpi_preempt_enable()
#define preempt_enable_no_resched() lkpi_preempt_enable()
#define preempt_disable_notrace() lkpi_preempt_disable()
#define preempt_enable_notrace() lkpi_preempt_enable()
#define preempt_check_resched() do { } while (0)
#define preempt_count() lkpi_preempt_count()

#define in_nmi() 0
#define in_hardirq() (lkpi_hardirq_count() != 0)
#define in_irq() in_hardirq()
#define in_serving_softirq() lkpi_in_serving_softirq()
#define in_softirq() in_serving_softirq()
#define in_interrupt() (in_hardirq() || in_softirq())
#define in_task() (!in_interrupt())
#define irq_count() (hardirq_count() | (in_softirq() ? SOFTIRQ_OFFSET : 0))
#define in_atomic() (preempt_count() != 0)
#define hardirq_count() (lkpi_hardirq_count() * HARDIRQ_OFFSET)
#define preemptible() (preempt_count() == 0 && !irqs_disabled())

#define migrate_disable() preempt_disable()
#define migrate_enable() preempt_enable()

#endif
