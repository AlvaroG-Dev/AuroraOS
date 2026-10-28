/* SPDX-License-Identifier: GPL-2.0-only */
/* Media movil exponencial en punto fijo (interfaz DECLARE_EWMA de Linux). */
#ifndef _LKPI_LINUX_AVERAGE_H
#define _LKPI_LINUX_AVERAGE_H

#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/log2.h>
#include <linux/types.h>

#define DECLARE_EWMA(name, _precision, _weight_rcp) \
	struct ewma_##name { unsigned long internal; }; \
	static inline void ewma_##name##_init(struct ewma_##name *e) \
	{ \
		BUILD_BUG_ON(!__builtin_constant_p(_precision)); \
		BUILD_BUG_ON(!__builtin_constant_p(_weight_rcp)); \
		BUILD_BUG_ON((_precision) > 30); \
		BUILD_BUG_ON_NOT_POWER_OF_2(_precision); \
		BUILD_BUG_ON_NOT_POWER_OF_2(_weight_rcp); \
		e->internal = 0; \
	} \
	static inline unsigned long ewma_##name##_read(struct ewma_##name *e) \
	{ \
		return e->internal >> (_precision); \
	} \
	static inline void ewma_##name##_add(struct ewma_##name *e, unsigned long val) \
	{ \
		unsigned long internal = READ_ONCE(e->internal); \
		unsigned long weight_rcp = ilog2(_weight_rcp); \
		unsigned long precision = _precision; \
		WRITE_ONCE(e->internal, internal ? \
			(((internal << weight_rcp) - internal) + (val << precision)) >> weight_rcp : \
			(val << precision)); \
	}

#endif
