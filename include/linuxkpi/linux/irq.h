/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_IRQ_H
#define _LKPI_LINUX_IRQ_H
#include <linux/interrupt.h>
/* Sin dominios de IRQ: los pocos usos (GPIO de i915 en Atom) no se alcanzan. */
struct irq_chip { const char *name; };
static inline void handle_simple_irq(struct irq_desc *desc) {}
#endif
