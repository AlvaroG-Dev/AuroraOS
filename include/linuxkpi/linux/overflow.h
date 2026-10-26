/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_OVERFLOW_H
#define _LKPI_LINUX_OVERFLOW_H

#include <linux/compiler.h>
#include <linux/limits.h>

#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_sub_overflow(a, b, d) __builtin_sub_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
#define check_shl_overflow(a, s, d) \
	({ typeof(a) _a = a; typeof(s) _s = s; typeof(d) _d = d; \
	   u64 _a_full = _a; unsigned int _to_shift = \
		(_s >= 0 && _s < 8 * sizeof(*d)) ? _s : 0; \
	   *_d = (_a_full << _to_shift); \
	   (_to_shift != _s || *_d < 0 || _a < 0 || (*_d >> _to_shift) != _a); })
#define overflows_type(n, T) \
	({ typeof(T) __tmp; __builtin_add_overflow((n), 0, &__tmp); })
#define add_would_overflow(a, b) \
	({ typeof(a) __tmp; check_add_overflow(a, b, &__tmp); })

static inline size_t __must_check size_mul(size_t a, size_t b)
{
	size_t bytes;
	return check_mul_overflow(a, b, &bytes) ? SIZE_MAX : bytes;
}
static inline size_t __must_check size_add(size_t a, size_t b)
{
	size_t bytes;
	return check_add_overflow(a, b, &bytes) ? SIZE_MAX : bytes;
}
static inline size_t __must_check size_sub(size_t minuend, size_t subtrahend)
{
	size_t bytes;
	if (minuend == SIZE_MAX || subtrahend == SIZE_MAX ||
	    check_sub_overflow(minuend, subtrahend, &bytes))
		return SIZE_MAX;
	return bytes;
}

#define array_size(a, b) size_mul(a, b)
#define array3_size(a, b, c) size_mul(size_mul(a, b), c)
#define flex_array_size(p, member, count) \
	size_mul(count, sizeof(*(p)->member) + __must_be_array((p)->member))
#define struct_size(p, member, count) \
	size_add(sizeof(*(p)), flex_array_size(p, member, count))
#define struct_size_t(type, member, count) struct_size((type *)NULL, member, count)

#endif
