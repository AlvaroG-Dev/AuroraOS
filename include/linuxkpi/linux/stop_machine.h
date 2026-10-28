/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_STOP_MACHINE_H
#define _LKPI_LINUX_STOP_MACHINE_H
/*
 * Aproximacion: ejecuta fn con las interrupciones desactivadas en la CPU
 * actual; NO detiene las demas CPU. Vale para i915 (que lo usa para
 * actualizar la GGTT en BXT/Gen8 con la GPU parada), no para un uso general.
 */
#include <linux/cpumask.h>
#include <linux/irqflags.h>
typedef int (*cpu_stop_fn_t)(void *arg);
static inline int stop_machine(cpu_stop_fn_t fn, void *data, const struct cpumask *cpus)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = fn(data);
	local_irq_restore(flags);
	return ret;
}
#endif
