/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_IRQRETURN_H
#define _LKPI_LINUX_IRQRETURN_H

/* Mismos valores que irqreturn_t de Aurora (kernel/irq_threaded.h). */
enum irqreturn {
	IRQ_NONE = (0 << 0),
	IRQ_HANDLED = (1 << 0),
	IRQ_WAKE_THREAD = (1 << 1),
};
typedef enum irqreturn irqreturn_t;
#define IRQ_RETVAL(x) ((x) ? IRQ_HANDLED : IRQ_NONE)

#endif
