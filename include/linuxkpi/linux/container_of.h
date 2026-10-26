/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_CONTAINER_OF_H
#define _LKPI_LINUX_CONTAINER_OF_H

#include <linux/build_bug.h>
#include <linux/stddef.h>

#define typeof_member(T, m) typeof(((T *)0)->m)

#define container_of(ptr, type, member) \
	({ void *__mptr = (void *)(ptr); \
	   static_assert(__same_type(*(ptr), ((type *)0)->member) || \
			 __same_type(*(ptr), void), "puntero de tipo incorrecto"); \
	   ((type *)(__mptr - offsetof(type, member))); })

#define container_of_const(ptr, type, member) \
	_Generic(ptr, \
		const typeof(*(ptr)) *: ((const type *)container_of(ptr, type, member)), \
		default: ((type *)container_of(ptr, type, member)))

#endif
