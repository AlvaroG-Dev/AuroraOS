/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_IRQ_H
#define _LKPI_LINUX_IRQ_H
#include <linux/interrupt.h>
/*
 * Sin dominios ni descriptores de IRQ virtuales (los usa el GSC de DG2/MTL,
 * que no se alcanza): irq_alloc_desc falla y el resto no hace nada.
 */
struct irq_desc;
struct irq_data { unsigned int irq; void *chip_data; };
struct irq_chip {
	const char *name;
	void (*irq_mask)(struct irq_data *data);
	void (*irq_unmask)(struct irq_data *data);
};
typedef void (*irq_flow_handler_t)(struct irq_desc *desc);
static inline void handle_simple_irq(struct irq_desc *desc) {}
static inline void irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip,
						 irq_flow_handler_t handle, const char *name) {}
static inline int irq_set_chip_data(unsigned int irq, void *data) { return 0; }
static inline int irq_alloc_desc(int node) { return -ENOSYS; }
static inline void irq_free_desc(unsigned int irq) {}
static inline int generic_handle_irq(unsigned int irq) { return -EINVAL; }
static inline int generic_handle_irq_safe(unsigned int irq) { return -EINVAL; }
#endif
