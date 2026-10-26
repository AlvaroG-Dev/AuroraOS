/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copias con el espacio de usuario (sobre copy_{to,from}_user de Aurora).
 * Como en Linux devuelven los bytes NO copiados (0 si todo fue bien).
 */
#ifndef _LKPI_LINUX_UACCESS_H
#define _LKPI_LINUX_UACCESS_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/gfp_types.h>

#define TASK_SIZE_MAX 0x00007FFFFFFFF000UL

unsigned long lkpi_copy_to_user(void __user *to, const void *from, unsigned long n);
unsigned long lkpi_copy_from_user(void *to, const void __user *from, unsigned long n);
long lkpi_strncpy_from_user(char *dst, const char __user *src, long count);

static inline bool access_ok(const void __user *addr, unsigned long size)
{
	unsigned long a = (unsigned long)addr;
	return a <= TASK_SIZE_MAX && size <= TASK_SIZE_MAX - a;
}
static inline unsigned long __must_check copy_to_user(void __user *to, const void *from,
						      unsigned long n)
{
	return lkpi_copy_to_user(to, from, n);
}
static inline unsigned long __must_check copy_from_user(void *to, const void __user *from,
							unsigned long n)
{
	return lkpi_copy_from_user(to, from, n);
}
#define __copy_to_user copy_to_user
#define __copy_from_user copy_from_user
#define raw_copy_to_user copy_to_user
#define raw_copy_from_user copy_from_user
static inline long strncpy_from_user(char *dst, const char __user *src, long count)
{
	return lkpi_strncpy_from_user(dst, src, count);
}
#define put_user(x, ptr) \
	({ __typeof__(*(ptr)) __pu_val = (x); \
	   copy_to_user((ptr), &__pu_val, sizeof(*(ptr))) ? -EFAULT : 0; })
#define get_user(x, ptr) \
	({ __typeof__(*(ptr)) __gu_val; int __gu_err = \
		copy_from_user(&__gu_val, (ptr), sizeof(*(ptr))) ? -EFAULT : 0; \
	   (x) = __gu_err ? (__typeof__(*(ptr)))0 : __gu_val; __gu_err; })
#define __put_user put_user
#define __get_user get_user
static inline unsigned long clear_user(void __user *to, unsigned long n)
{
	static const u8 zeros[256];
	unsigned long left = n;
	while (left) {
		unsigned long c = left < sizeof(zeros) ? left : sizeof(zeros);
		if (copy_to_user((u8 __user *)to + (n - left), zeros, c))
			return left;
		left -= c;
	}
	return 0;
}
void *lkpi_memdup_user(const void __user *src, size_t len);
#define memdup_user lkpi_memdup_user
#define vmemdup_user lkpi_memdup_user
#define pagefault_disable() do { } while (0)
#define pagefault_enable() do { } while (0)
#define user_access_begin(p, n) access_ok(p, n)
#define user_access_end() do { } while (0)
#define user_read_access_begin user_access_begin
#define user_read_access_end user_access_end
#define user_write_access_begin user_access_begin
#define user_write_access_end user_access_end

#endif
