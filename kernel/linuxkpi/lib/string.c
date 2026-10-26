// SPDX-License-Identifier: GPL-2.0-only
/* Funciones de cadena y conversion numerica de la capa LinuxKPI. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/string.h>

size_t lkpi_strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return p - s;
}
EXPORT_SYMBOL(lkpi_strlen);

size_t lkpi_strnlen(const char *s, size_t max)
{
	size_t n = 0;
	while (n < max && s[n])
		n++;
	return n;
}
EXPORT_SYMBOL(lkpi_strnlen);

int lkpi_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a < (unsigned char)*b ? -1 : (unsigned char)*a > (unsigned char)*b;
}
EXPORT_SYMBOL(lkpi_strcmp);

int lkpi_strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
		if (!*a)
			break;
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_strncmp);

int lkpi_strncasecmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca - cb;
		if (!ca)
			break;
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_strncasecmp);

int lkpi_strcasecmp(const char *a, const char *b)
{
	return lkpi_strncasecmp(a, b, SIZE_MAX);
}
EXPORT_SYMBOL(lkpi_strcasecmp);

char *lkpi_strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}
EXPORT_SYMBOL(lkpi_strcpy);

char *lkpi_strncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = '\0';
	return d;
}
EXPORT_SYMBOL(lkpi_strncpy);

char *lkpi_strcat(char *d, const char *s)
{
	lkpi_strcpy(d + lkpi_strlen(d), s);
	return d;
}
EXPORT_SYMBOL(lkpi_strcat);

char *lkpi_strncat(char *d, const char *s, size_t n)
{
	char *p = d + lkpi_strlen(d);
	while (n-- && *s)
		*p++ = *s++;
	*p = '\0';
	return d;
}
EXPORT_SYMBOL(lkpi_strncat);

size_t lkpi_strlcpy(char *d, const char *s, size_t size)
{
	size_t len = lkpi_strlen(s);
	if (size) {
		size_t n = len >= size ? size - 1 : len;
		memcpy(d, s, n);
		d[n] = '\0';
	}
	return len;
}
EXPORT_SYMBOL(lkpi_strlcpy);

size_t lkpi_strlcat(char *d, const char *s, size_t size)
{
	size_t dl = lkpi_strnlen(d, size);
	if (dl == size)
		return dl + lkpi_strlen(s);
	return dl + lkpi_strlcpy(d + dl, s, size - dl);
}
EXPORT_SYMBOL(lkpi_strlcat);

/* Copia con terminacion garantizada; -E2BIG si se trunca. */
ssize_t lkpi_strscpy(char *d, const char *s, size_t size)
{
	size_t i;
	if (size == 0 || size > INT_MAX)
		return -E2BIG;
	for (i = 0; i < size; i++) {
		d[i] = s[i];
		if (!s[i])
			return i;
	}
	d[size - 1] = '\0';
	return -E2BIG;
}
EXPORT_SYMBOL(lkpi_strscpy);

ssize_t lkpi_strscpy_pad(char *d, const char *s, size_t size)
{
	ssize_t n = lkpi_strscpy(d, s, size);
	if (n >= 0 && (size_t)n + 1 < size)
		memset(d + n + 1, 0, size - n - 1);
	return n;
}
EXPORT_SYMBOL(lkpi_strscpy_pad);

char *lkpi_strchr(const char *s, int c)
{
	for (; *s != (char)c; s++)
		if (!*s)
			return NULL;
	return (char *)s;
}
EXPORT_SYMBOL(lkpi_strchr);

char *lkpi_strchrnul(const char *s, int c)
{
	while (*s && *s != (char)c)
		s++;
	return (char *)s;
}
EXPORT_SYMBOL(lkpi_strchrnul);

char *lkpi_strnchr(const char *s, size_t n, int c)
{
	for (; n-- && *s; s++)
		if (*s == (char)c)
			return (char *)s;
	return NULL;
}
EXPORT_SYMBOL(lkpi_strnchr);

char *lkpi_strrchr(const char *s, int c)
{
	const char *last = NULL;
	do {
		if (*s == (char)c)
			last = s;
	} while (*s++);
	return (char *)last;
}
EXPORT_SYMBOL(lkpi_strrchr);

char *lkpi_strnstr(const char *h, const char *n, size_t len)
{
	size_t l2 = lkpi_strlen(n);
	if (!l2)
		return (char *)h;
	while (len >= l2) {
		len--;
		if (!memcmp(h, n, l2))
			return (char *)h;
		h++;
	}
	return NULL;
}
EXPORT_SYMBOL(lkpi_strnstr);

char *lkpi_strstr(const char *h, const char *n)
{
	return lkpi_strnstr(h, n, lkpi_strlen(h));
}
EXPORT_SYMBOL(lkpi_strstr);

char *lkpi_strpbrk(const char *s, const char *ct)
{
	for (; *s; s++)
		if (lkpi_strchr(ct, *s))
			return (char *)s;
	return NULL;
}
EXPORT_SYMBOL(lkpi_strpbrk);

char *lkpi_strsep(char **s, const char *ct)
{
	char *sbegin = *s, *end;
	if (!sbegin)
		return NULL;
	end = lkpi_strpbrk(sbegin, ct);
	if (end)
		*end++ = '\0';
	*s = end;
	return sbegin;
}
EXPORT_SYMBOL(lkpi_strsep);

size_t lkpi_strspn(const char *s, const char *accept)
{
	size_t n = 0;
	while (s[n] && lkpi_strchr(accept, s[n]))
		n++;
	return n;
}
EXPORT_SYMBOL(lkpi_strspn);

size_t lkpi_strcspn(const char *s, const char *reject)
{
	size_t n = 0;
	while (s[n] && !lkpi_strchr(reject, s[n]))
		n++;
	return n;
}
EXPORT_SYMBOL(lkpi_strcspn);

void *lkpi_memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n--; p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}
EXPORT_SYMBOL(lkpi_memchr);

/* Primer byte distinto de c, o NULL si todos lo son. */
void *lkpi_memchr_inv(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n--; p++)
		if (*p != (unsigned char)c)
			return (void *)p;
	return NULL;
}
EXPORT_SYMBOL(lkpi_memchr_inv);

void *lkpi_memscan(void *addr, int c, size_t size)
{
	unsigned char *p = addr;
	while (size && *p != (unsigned char)c) {
		p++;
		size--;
	}
	return p;
}
EXPORT_SYMBOL(lkpi_memscan);

char *lkpi_skip_spaces(const char *str)
{
	while (isspace(*str))
		str++;
	return (char *)str;
}
EXPORT_SYMBOL(lkpi_skip_spaces);

char *lkpi_strim(char *s)
{
	size_t size = lkpi_strlen(s);
	char *end;
	if (!size)
		return s;
	end = s + size - 1;
	while (end >= s && isspace(*end))
		end--;
	*(end + 1) = '\0';
	return lkpi_skip_spaces(s);
}
EXPORT_SYMBOL(lkpi_strim);

/* Igualdad ignorando un '\n' final en cualquiera de las dos. */
bool lkpi_sysfs_streq(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	if (*s1 == *s2)
		return true;
	if (!*s1 && *s2 == '\n' && !s2[1])
		return true;
	if (*s1 == '\n' && !s1[1] && !*s2)
		return true;
	return false;
}
EXPORT_SYMBOL(lkpi_sysfs_streq);

int lkpi_match_string(const char *const *array, size_t n, const char *string)
{
	for (size_t i = 0; i < n; i++) {
		if (!array[i])
			break;
		if (!lkpi_strcmp(array[i], string))
			return i;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(lkpi_match_string);

char *lkpi_strreplace(char *str, char old, char new)
{
	char *s = str;
	for (; *s; s++)
		if (*s == old)
			*s = new;
	return s;
}
EXPORT_SYMBOL(lkpi_strreplace);

char *lkpi_kstrdup(const char *s, gfp_t gfp)
{
	size_t len;
	char *p;
	if (!s)
		return NULL;
	len = lkpi_strlen(s) + 1;
	p = kmalloc(len, gfp);
	if (p)
		memcpy(p, s, len);
	return p;
}
EXPORT_SYMBOL(lkpi_kstrdup);

char *lkpi_kstrndup(const char *s, size_t max, gfp_t gfp)
{
	size_t len;
	char *p;
	if (!s)
		return NULL;
	len = lkpi_strnlen(s, max);
	p = kmalloc(len + 1, gfp);
	if (p) {
		memcpy(p, s, len);
		p[len] = '\0';
	}
	return p;
}
EXPORT_SYMBOL(lkpi_kstrndup);

void *lkpi_kmemdup(const void *src, size_t len, gfp_t gfp)
{
	void *p = kmalloc(len, gfp);
	if (p)
		memcpy(p, src, len);
	return p;
}
EXPORT_SYMBOL(lkpi_kmemdup);

char *lkpi_kmemdup_nul(const char *s, size_t len, gfp_t gfp)
{
	char *p;
	if (!s)
		return NULL;
	p = kmalloc(len + 1, gfp);
	if (p) {
		memcpy(p, s, len);
		p[len] = '\0';
	}
	return p;
}
EXPORT_SYMBOL(lkpi_kmemdup_nul);

/* Siempre duplica (no distinguimos .rodata), asi que kfree_const libera. */
const char *lkpi_kstrdup_const(const char *s, gfp_t gfp)
{
	return lkpi_kstrdup(s, gfp);
}
EXPORT_SYMBOL(lkpi_kstrdup_const);

void lkpi_kfree_const(const void *x)
{
	kfree(x);
}
EXPORT_SYMBOL(lkpi_kfree_const);

/* Parte str en palabras separadas por espacios; vector terminado en NULL. */
char **lkpi_argv_split(gfp_t gfp, const char *str, int *argcp)
{
	char *copy = lkpi_kstrdup(str, gfp), *p, **argv;
	int argc = 0, i = 0;

	if (!copy)
		return NULL;
	for (p = lkpi_skip_spaces(copy); *p; argc++) {
		while (*p && !isspace(*p))
			p++;
		p = lkpi_skip_spaces(p);
	}
	argv = kmalloc_array(argc + 2, sizeof(*argv), gfp);
	if (!argv) {
		kfree(copy);
		return NULL;
	}
	/* argv[argc + 1] guarda la copia para argv_free(). */
	for (p = lkpi_skip_spaces(copy); *p;) {
		argv[i++] = p;
		while (*p && !isspace(*p))
			p++;
		if (*p)
			*p++ = '\0';
		p = lkpi_skip_spaces(p);
	}
	argv[i] = NULL;
	argv[argc + 1] = copy;
	if (argcp)
		*argcp = argc;
	return argv;
}
EXPORT_SYMBOL(lkpi_argv_split);

void lkpi_argv_free(char **argv)
{
	int argc = 0;
	if (!argv)
		return;
	while (argv[argc])
		argc++;
	kfree(argv[argc + 1]);
	kfree(argv);
}
EXPORT_SYMBOL(lkpi_argv_free);

/* ----------------------------------------------------- hex y numeros */

int lkpi_hex2bin(u8 *dst, const char *src, size_t count)
{
	while (count--) {
		int hi = hex_to_bin(*src++), lo;
		if (hi < 0)
			return -EINVAL;
		lo = hex_to_bin(*src++);
		if (lo < 0)
			return -EINVAL;
		*dst++ = (hi << 4) | lo;
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_hex2bin);

char *lkpi_bin2hex(char *dst, const void *src, size_t count)
{
	const unsigned char *s = src;
	while (count--)
		dst = hex_byte_pack(dst, *s++);
	return dst;
}
EXPORT_SYMBOL(lkpi_bin2hex);

/* Base 0: autodetecta 0x (16) y 0 (8). */
static const char *parse_base(const char *s, unsigned int *base)
{
	if (*base == 0) {
		if (s[0] == '0') {
			if (_tolower(s[1]) == 'x' && isxdigit(s[2]))
				*base = 16;
			else
				*base = 8;
		} else {
			*base = 10;
		}
	}
	if (*base == 16 && s[0] == '0' && _tolower(s[1]) == 'x')
		s += 2;
	return s;
}

/* Devuelve el numero de digitos consumidos; *overflow si no cabe. */
static unsigned int parse_ull(const char *s, unsigned int base, unsigned long long *p,
			      bool *overflow)
{
	unsigned long long res = 0;
	unsigned int rv = 0;
	*overflow = false;
	for (;; s++, rv++) {
		unsigned int val;
		char c = *s;
		if ('0' <= c && c <= '9')
			val = c - '0';
		else if ('a' <= _tolower(c) && _tolower(c) <= 'f')
			val = _tolower(c) - 'a' + 10;
		else
			break;
		if (val >= base)
			break;
		if (res > (ULLONG_MAX - val) / base)
			*overflow = true;
		res = res * base + val;
	}
	*p = res;
	return rv;
}

int lkpi_kstrtoull(const char *s, unsigned int base, unsigned long long *res)
{
	unsigned long long r;
	unsigned int rv;
	bool overflow;

	if (s[0] == '+')
		s++;
	s = parse_base(s, &base);
	rv = parse_ull(s, base, &r, &overflow);
	if (overflow)
		return -ERANGE;
	if (rv == 0)
		return -EINVAL;
	s += rv;
	if (*s == '\n')
		s++;
	if (*s)
		return -EINVAL;
	*res = r;
	return 0;
}
EXPORT_SYMBOL(lkpi_kstrtoull);

int lkpi_kstrtoll(const char *s, unsigned int base, long long *res)
{
	unsigned long long tmp;
	int rv;

	if (s[0] == '-') {
		rv = lkpi_kstrtoull(s + 1, base, &tmp);
		if (rv < 0)
			return rv;
		if ((long long)-tmp > 0)
			return -ERANGE;
		*res = -tmp;
	} else {
		rv = lkpi_kstrtoull(s, base, &tmp);
		if (rv < 0)
			return rv;
		if ((long long)tmp < 0)
			return -ERANGE;
		*res = tmp;
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_kstrtoll);

int lkpi_kstrtobool(const char *s, bool *res)
{
	if (!s)
		return -EINVAL;
	switch (s[0]) {
	case 'y': case 'Y': case 't': case 'T': case '1':
		*res = true;
		return 0;
	case 'n': case 'N': case 'f': case 'F': case '0':
		*res = false;
		return 0;
	case 'o': case 'O':
		if (_tolower(s[1]) == 'n') {
			*res = true;
			return 0;
		}
		if (_tolower(s[1]) == 'f') {
			*res = false;
			return 0;
		}
		break;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(lkpi_kstrtobool);

unsigned long long lkpi_simple_strtoull(const char *cp, char **endp, unsigned int base)
{
	unsigned long long result;
	bool overflow;
	unsigned int rv;

	cp = parse_base(cp, &base);
	rv = parse_ull(cp, base, &result, &overflow);
	if (endp)
		*endp = (char *)cp + rv;
	return result;
}
EXPORT_SYMBOL(lkpi_simple_strtoull);

long long lkpi_simple_strtoll(const char *cp, char **endp, unsigned int base)
{
	if (*cp == '-')
		return -(long long)lkpi_simple_strtoull(cp + 1, endp, base);
	return lkpi_simple_strtoull(cp, endp, base);
}
EXPORT_SYMBOL(lkpi_simple_strtoll);
