/* SPDX-License-Identifier: GPL-2.0-only */
/* stackdepot de la capa: sin trazas de pila, los asas son 0 y no hay nada que mostrar. */
#ifndef _LKPI_LINUX_STACKDEPOT_H
#define _LKPI_LINUX_STACKDEPOT_H

#include <linux/types.h>
#include <linux/gfp.h>

typedef u32 depot_stack_handle_t;

static inline depot_stack_handle_t stack_depot_save(unsigned long *entries,
						    unsigned int nr_entries, gfp_t gfp)
{
	return 0;
}

static inline unsigned int stack_depot_fetch(depot_stack_handle_t handle, unsigned long **entries)
{
	*entries = NULL;
	return 0;
}

static inline int stack_depot_snprint(depot_stack_handle_t handle, char *buf, size_t size,
				      int spaces)
{
	if (size)
		buf[0] = '\0';
	return 0;
}

static inline void stack_depot_print(depot_stack_handle_t stack) { }
static inline int stack_depot_init(void) { return 0; }

#endif
