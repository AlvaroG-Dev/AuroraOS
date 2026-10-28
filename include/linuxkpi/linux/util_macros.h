/* SPDX-License-Identifier: GPL-2.0-only */
/* Macros de utilidad: for_each_if, PTR_IF, u64_to_user_ptr, find_closest. */
#ifndef _LKPI_LINUX_UTIL_MACROS_H
#define _LKPI_LINUX_UTIL_MACROS_H

#include <linux/compiler_attributes.h>
#include <linux/math.h>
#include <linux/typecheck.h>
#include <linux/stddef.h>

/* for_each_foo(x) for_each_if(cond): el cuerpo solo se ejecuta si cond */
#define for_each_if(condition) if (!(condition)) {} else

#define PTR_IF(cond, ptr)	((cond) ? (ptr) : NULL)

#ifndef u64_to_user_ptr
#define u64_to_user_ptr(x)		\
({					\
	typecheck(u64, (x));		\
	(void __user *)(uintptr_t)(x);	\
})
#endif

#define is_insidevar(ptr, var)						\
	((uintptr_t)(ptr) >= (uintptr_t)(var) &&			\
	 (uintptr_t)(ptr) <  (uintptr_t)(var) + sizeof(var))

/* Indice del elemento de la tabla ordenada (ascendente) mas cercano a x */
#define find_closest(x, a, as)						\
({									\
	typeof(as) __fc_i, __fc_as = (as) - 1;				\
	long __fc_x = (x), __fc_mid, __fc_l, __fc_r;			\
	typeof(*a) const *__fc_a = (a);					\
	for (__fc_i = 0; __fc_i < __fc_as; __fc_i++) {			\
		__fc_mid = (__fc_a[__fc_i] + __fc_a[__fc_i + 1]) / 2;	\
		if (__fc_x <= __fc_mid) {				\
			__fc_l = __fc_x - __fc_a[__fc_i];		\
			__fc_r = __fc_a[__fc_i + 1] - __fc_x;		\
			if (__fc_r < __fc_l)				\
				__fc_i++;				\
			break;						\
		}							\
	}								\
	(__fc_i);							\
})

#endif
