/* SPDX-License-Identifier: GPL-2.0-only */
/* Trazas de pila: la capa no las recoge. */
#ifndef _LKPI_LINUX_STACKTRACE_H
#define _LKPI_LINUX_STACKTRACE_H

#include <linux/types.h>

static inline unsigned int stack_trace_save(unsigned long *store, unsigned int size,
					    unsigned int skipnr)
{
	return 0;
}

static inline void stack_trace_print(const unsigned long *trace, unsigned int nr_entries,
				     int spaces) { }

#endif
