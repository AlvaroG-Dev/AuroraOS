// SPDX-License-Identifier: GPL-2.0-only
/* Copias con el espacio de usuario sobre las de Aurora (semantica de Linux). */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "../aurora.h"

unsigned long lkpi_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	if (!n)
		return 0;
	if (!access_ok(to, n))
		return n;
	return aurora_copy_to_user((void __force *)to, from, n) ? n : 0;
}
EXPORT_SYMBOL(lkpi_copy_to_user);

unsigned long lkpi_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	if (!n)
		return 0;
	if (!access_ok(from, n) || aurora_copy_from_user(to, (const void __force *)from, n)) {
		memset(to, 0, n);	/* como Linux: lo no copiado queda a cero */
		return n;
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_copy_from_user);

/*
 * Linux: devuelve la longitud sin el NUL, o count si no hubo NUL en count
 * bytes (y entonces dst no queda terminado), o -EFAULT.
 */
long lkpi_strncpy_from_user(char *dst, const char __user *src, long count)
{
	long i;

	for (i = 0; i < count; i++) {
		char c;
		if (lkpi_copy_from_user(&c, src + i, 1))
			return -EFAULT;
		dst[i] = c;
		if (!c)
			return i;
	}
	return count;
}
EXPORT_SYMBOL(lkpi_strncpy_from_user);

void *lkpi_memdup_user(const void __user *src, size_t len)
{
	void *p = kmalloc(len, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(p, src, len)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	return p;
}
EXPORT_SYMBOL(lkpi_memdup_user);
