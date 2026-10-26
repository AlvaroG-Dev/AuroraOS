/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Anotaciones de tipos del kernel. Se incluye siempre (-include). Las de
 * sparse (__user, __iomem, __rcu...) no tienen efecto al compilar.
 */
#ifndef _LKPI_LINUX_COMPILER_TYPES_H
#define _LKPI_LINUX_COMPILER_TYPES_H

#ifndef __ASSEMBLY__
#include <linux/compiler_attributes.h>

#define __user
#define __kernel
#define __iomem
#define __percpu
#define __rcu
#define __force
#define __nocast
#define __safe
#define __private
#define __bitwise
#define __must_hold(x)
#define __acquires(x)
#define __releases(x)
#define __acquire(x) (void)0
#define __release(x) (void)0
#define __cond_lock(x, c) (c)
#define __cond_acquires(x)
#define ACCESS_PRIVATE(p, member) ((p)->member)
#define __builtin_warning(x, y...) (1)

#define __init_or_module
#define notrace
#define __naked __attribute__((__naked__)) notrace
#define __randomize_layout
#define __no_randomize_layout
#define randomized_struct_fields_start struct {
#define randomized_struct_fields_end };
#define __latent_entropy
#define __nocfi
#define __noscs
#define __ro_after_init
#define __read_mostly
#define __cacheline_aligned __aligned(64)
#define __cacheline_aligned_in_smp __aligned(64)
#define ____cacheline_aligned __aligned(64)
#define ____cacheline_aligned_in_smp __aligned(64)
#define __rcu_guarded

#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

/* Tipo escalar sin const/volatile (como Linux). */
#define __scalar_type_to_expr_cases(type) \
	unsigned type : (unsigned type)0, signed type : (signed type)0
#define __unqual_scalar_typeof(x) typeof(_Generic((x), \
	char : (char)0, \
	__scalar_type_to_expr_cases(char), \
	__scalar_type_to_expr_cases(short), \
	__scalar_type_to_expr_cases(int), \
	__scalar_type_to_expr_cases(long), \
	__scalar_type_to_expr_cases(long long), \
	default : (x)))

#define compiletime_assert(cond, msg) _Static_assert(cond, msg)
#define compiletime_assert_atomic_type(t) \
	compiletime_assert(sizeof(t) <= sizeof(long), "tipo no atomico")
#define __native_word(t) \
	(sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || \
	 sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))

#define inline inline __attribute__((__gnu_inline__)) __attribute__((__unused__))
#define __inline__ inline
#define __inline inline

#endif /* __ASSEMBLY__ */
#endif
