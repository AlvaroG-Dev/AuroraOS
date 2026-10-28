/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_MINMAX_H
#define _LKPI_LINUX_MINMAX_H

#include <linux/compiler.h>

/*
 * min/max evaluan cada argumento una vez. A diferencia de Linux no se
 * rechazan en compilacion las comparaciones con signo distinto; se avisa con
 * -Wsign-compare si se activa.
 */
#define __cmp_op_min <
#define __cmp_op_max >
#define __cmp(op, x, y) ((x) __cmp_op_##op (y) ? (x) : (y))
#define __careful_cmp(op, x, y) \
	({ typeof(x) __x = (x); typeof(y) __y = (y); __cmp(op, __x, __y); })

#define min(x, y) __careful_cmp(min, x, y)
#define max(x, y) __careful_cmp(max, x, y)
#define umin(x, y) min((x) + 0u + 0ul + 0ull, (y) + 0u + 0ul + 0ull)
#define umax(x, y) max((x) + 0u + 0ul + 0ull, (y) + 0u + 0ul + 0ull)
#define min3(x, y, z) min((typeof(x))min(x, y), z)
#define max3(x, y, z) max((typeof(x))max(x, y), z)
#define min_not_zero(x, y) \
	({ typeof(x) __x = (x); typeof(y) __y = (y); \
	   __x == 0 ? __y : ((__y == 0) ? __x : min(__x, __y)); })
#define clamp(val, lo, hi) min((typeof(val))max(val, lo), hi)
#define min_t(type, x, y) __careful_cmp(min, (type)(x), (type)(y))
#define max_t(type, x, y) __careful_cmp(max, (type)(x), (type)(y))
#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)
#define clamp_val(val, lo, hi) clamp_t(typeof(val), val, lo, hi)
#define min_array(arr, len) \
	({ typeof(&(arr)[0]) __a = (arr); size_t __l = (len); \
	   typeof((arr)[0]) __m = __a[0]; \
	   for (size_t __i = 1; __i < __l; __i++) if (__a[__i] < __m) __m = __a[__i]; \
	   __m; })
#define max_array(arr, len) \
	({ typeof(&(arr)[0]) __a = (arr); size_t __l = (len); \
	   typeof((arr)[0]) __m = __a[0]; \
	   for (size_t __i = 1; __i < __l; __i++) if (__a[__i] > __m) __m = __a[__i]; \
	   __m; })
#define in_range(val, start, len) \
	((val) >= (start) && (val) - (start) < (len))

#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

/* Versiones constantes (aptas para tamanos de array) */
#define MIN_T(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define MAX_T(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))

#endif
