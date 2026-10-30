/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_KSTRTOX_H
#define _LKPI_LINUX_KSTRTOX_H

#include <linux/compiler.h>
#include <linux/types.h>

int __must_check lkpi_kstrtoull(const char *s, unsigned int base, unsigned long long *res);
int __must_check lkpi_kstrtoll(const char *s, unsigned int base, long long *res);
unsigned long long lkpi_simple_strtoull(const char *cp, char **endp, unsigned int base);
long long lkpi_simple_strtoll(const char *cp, char **endp, unsigned int base);
int __must_check lkpi_kstrtobool(const char *s, bool *res);

static inline int __must_check kstrtoull(const char *s, unsigned int base, unsigned long long *res)
{
	return lkpi_kstrtoull(s, base, res);
}
static inline int __must_check kstrtoll(const char *s, unsigned int base, long long *res)
{
	return lkpi_kstrtoll(s, base, res);
}
static inline int __must_check kstrtoul(const char *s, unsigned int base, unsigned long *res)
{
	return lkpi_kstrtoull(s, base, (unsigned long long *)res);
}
static inline int __must_check kstrtol(const char *s, unsigned int base, long *res)
{
	return lkpi_kstrtoll(s, base, (long long *)res);
}
#define __LKPI_KSTRTO(name, type, wide, wtype, lo, hi) \
static inline int __must_check name(const char *s, unsigned int base, type *res) \
{ \
	wtype tmp; int rc = wide(s, base, &tmp); \
	if (rc) return rc; \
	if (tmp < (wtype)(lo) || tmp > (wtype)(hi)) return -34 /* ERANGE */; \
	*res = (type)tmp; return 0; \
}
__LKPI_KSTRTO(kstrtouint, unsigned int, lkpi_kstrtoull, unsigned long long, 0, 0xffffffffULL)
__LKPI_KSTRTO(kstrtoint, int, lkpi_kstrtoll, long long, -2147483648LL, 2147483647LL)
__LKPI_KSTRTO(kstrtou16, u16, lkpi_kstrtoull, unsigned long long, 0, 0xffff)
__LKPI_KSTRTO(kstrtos16, s16, lkpi_kstrtoll, long long, -32768, 32767)
__LKPI_KSTRTO(kstrtou8, u8, lkpi_kstrtoull, unsigned long long, 0, 0xff)
__LKPI_KSTRTO(kstrtos8, s8, lkpi_kstrtoll, long long, -128, 127)
#undef __LKPI_KSTRTO
static inline int __must_check kstrtou64(const char *s, unsigned int base, u64 *res)
{
	return kstrtoull(s, base, (unsigned long long *)res);
}
static inline int __must_check kstrtos64(const char *s, unsigned int base, s64 *res)
{
	return kstrtoll(s, base, (long long *)res);
}
static inline int __must_check kstrtou32(const char *s, unsigned int base, u32 *res)
{
	return kstrtouint(s, base, res);
}
static inline int __must_check kstrtos32(const char *s, unsigned int base, s32 *res)
{
	return kstrtoint(s, base, res);
}
static inline int __must_check kstrtobool(const char *s, bool *res)
{
	return lkpi_kstrtobool(s, res);
}
#define simple_strtoull lkpi_simple_strtoull
#define simple_strtoll lkpi_simple_strtoll
#define simple_strtoul(cp, endp, base) ((unsigned long)lkpi_simple_strtoull(cp, endp, base))
#define simple_strtol(cp, endp, base) ((long)lkpi_simple_strtoll(cp, endp, base))

/* Variantes desde memoria de usuario: L4 (K8). */


#include <linux/uaccess.h>
#include <linux/slab.h>
static inline int kstrtoull_from_user(const char __user *s, size_t count, unsigned int base, unsigned long long *res)
{
	char buf[24];

	count = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, s, count))
		return -EFAULT;
	buf[count] = '\0';
	return kstrtoull(buf, base, res);
}
#define __KSTRTO_FROM_USER(name, type, wide, wfn) \
static inline int name(const char __user *s, size_t count, unsigned int base, type *res) \
{ \
	wide tmp; \
	int ret = wfn(s, count, base, &tmp); \
	if (ret) \
		return ret; \
	if ((type)tmp != tmp) \
		return -ERANGE; \
	*res = tmp; \
	return 0; \
}
static inline int kstrtoll_from_user(const char __user *s, size_t count, unsigned int base, long long *res)
{
	char buf[24];

	count = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, s, count))
		return -EFAULT;
	buf[count] = '\0';
	return kstrtoll(buf, base, res);
}
__KSTRTO_FROM_USER(kstrtouint_from_user, unsigned int, unsigned long long, kstrtoull_from_user)
__KSTRTO_FROM_USER(kstrtoint_from_user, int, long long, kstrtoll_from_user)
__KSTRTO_FROM_USER(kstrtoul_from_user, unsigned long, unsigned long long, kstrtoull_from_user)
__KSTRTO_FROM_USER(kstrtol_from_user, long, long long, kstrtoll_from_user)
static inline int kstrtobool_from_user(const char __user *s, size_t count, bool *res)
{
	char buf[16];

	count = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, s, count))
		return -EFAULT;
	buf[count] = '\0';
	return kstrtobool(buf, res);
}
#endif
