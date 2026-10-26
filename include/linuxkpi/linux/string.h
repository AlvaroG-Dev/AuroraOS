/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Funciones de cadena. mem{cpy,set,move,cmp} van a los builtins de GCC (que
 * emiten llamadas a los simbolos del kernel de Aurora, los mismos que el
 * compilador puede generar por su cuenta); el resto las implementa la capa
 * (lkpi_*).
 */
#ifndef _LKPI_LINUX_STRING_H
#define _LKPI_LINUX_STRING_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/gfp_types.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
#define memcpy(d, s, n) __builtin_memcpy(d, s, n)
#define memset(d, c, n) __builtin_memset(d, c, n)
#define memmove(d, s, n) __builtin_memmove(d, s, n)
#define memcmp(a, b, n) __builtin_memcmp(a, b, n)

size_t lkpi_strlen(const char *s);
size_t lkpi_strnlen(const char *s, size_t max);
int lkpi_strcmp(const char *a, const char *b);
int lkpi_strncmp(const char *a, const char *b, size_t n);
int lkpi_strcasecmp(const char *a, const char *b);
int lkpi_strncasecmp(const char *a, const char *b, size_t n);
char *lkpi_strcpy(char *d, const char *s);
char *lkpi_strncpy(char *d, const char *s, size_t n);
char *lkpi_strcat(char *d, const char *s);
char *lkpi_strncat(char *d, const char *s, size_t n);
size_t lkpi_strlcpy(char *d, const char *s, size_t size);
size_t lkpi_strlcat(char *d, const char *s, size_t size);
ssize_t lkpi_strscpy(char *d, const char *s, size_t size);
ssize_t lkpi_strscpy_pad(char *d, const char *s, size_t size);
char *lkpi_strchr(const char *s, int c);
char *lkpi_strchrnul(const char *s, int c);
char *lkpi_strnchr(const char *s, size_t n, int c);
char *lkpi_strrchr(const char *s, int c);
char *lkpi_strstr(const char *h, const char *n);
char *lkpi_strnstr(const char *h, const char *n, size_t len);
char *lkpi_strpbrk(const char *s, const char *ct);
char *lkpi_strsep(char **s, const char *ct);
size_t lkpi_strspn(const char *s, const char *accept);
size_t lkpi_strcspn(const char *s, const char *reject);
void *lkpi_memchr(const void *s, int c, size_t n);
void *lkpi_memchr_inv(const void *s, int c, size_t n);
void *lkpi_memscan(void *addr, int c, size_t size);
char *lkpi_skip_spaces(const char *str);
char *lkpi_strim(char *s);
bool lkpi_sysfs_streq(const char *s1, const char *s2);
int lkpi_match_string(const char *const *array, size_t n, const char *string);
char *lkpi_strreplace(char *str, char old, char new);
char *lkpi_kstrdup(const char *s, gfp_t gfp);
char *lkpi_kstrndup(const char *s, size_t max, gfp_t gfp);
void *lkpi_kmemdup(const void *src, size_t len, gfp_t gfp);
char *lkpi_kmemdup_nul(const char *s, size_t len, gfp_t gfp);
const char *lkpi_kstrdup_const(const char *s, gfp_t gfp);
void lkpi_kfree_const(const void *x);
char **lkpi_argv_split(gfp_t gfp, const char *str, int *argcp);
void lkpi_argv_free(char **argv);

#define strlen(s) lkpi_strlen(s)
#define strnlen(s, m) lkpi_strnlen(s, m)
#define strcmp(a, b) lkpi_strcmp(a, b)
#define strncmp(a, b, n) lkpi_strncmp(a, b, n)
#define strcasecmp(a, b) lkpi_strcasecmp(a, b)
#define strncasecmp(a, b, n) lkpi_strncasecmp(a, b, n)
#define strcpy(d, s) lkpi_strcpy(d, s)
#define strncpy(d, s, n) lkpi_strncpy(d, s, n)
#define strcat(d, s) lkpi_strcat(d, s)
#define strncat(d, s, n) lkpi_strncat(d, s, n)
#define strlcpy(d, s, n) lkpi_strlcpy(d, s, n)
#define strlcat(d, s, n) lkpi_strlcat(d, s, n)
#define strchr(s, c) lkpi_strchr(s, c)
#define strchrnul(s, c) lkpi_strchrnul(s, c)
#define strnchr(s, n, c) lkpi_strnchr(s, n, c)
#define strrchr(s, c) lkpi_strrchr(s, c)
#define strstr(h, n) lkpi_strstr(h, n)
#define strnstr(h, n, l) lkpi_strnstr(h, n, l)
#define strpbrk(s, c) lkpi_strpbrk(s, c)
#define strsep(s, c) lkpi_strsep(s, c)
#define strspn(s, a) lkpi_strspn(s, a)
#define strcspn(s, r) lkpi_strcspn(s, r)
#define memchr(s, c, n) lkpi_memchr(s, c, n)
#define memchr_inv(s, c, n) lkpi_memchr_inv(s, c, n)
#define memscan(a, c, n) lkpi_memscan(a, c, n)
#define skip_spaces(s) lkpi_skip_spaces(s)
#define strim(s) lkpi_strim(s)
#define sysfs_streq(a, b) lkpi_sysfs_streq(a, b)
#define match_string(a, n, s) lkpi_match_string(a, n, s)
#define strreplace(s, o, n) lkpi_strreplace(s, o, n)
#define kstrdup(s, g) lkpi_kstrdup(s, g)
#define kstrndup(s, m, g) lkpi_kstrndup(s, m, g)
#define kmemdup(s, l, g) lkpi_kmemdup(s, l, g)
#define kmemdup_nul(s, l, g) lkpi_kmemdup_nul(s, l, g)
#define kstrdup_const(s, g) lkpi_kstrdup_const(s, g)
#define kfree_const(x) lkpi_kfree_const(x)
#define argv_split(g, s, c) lkpi_argv_split(g, s, c)
#define argv_free(a) lkpi_argv_free(a)

/* strscpy(dst, src) usa sizeof(dst) si no se da tamano (dst array). */
#define __strscpy_2(d, s) lkpi_strscpy(d, s, sizeof(d))
#define __strscpy_3(d, s, n) lkpi_strscpy(d, s, n)
#define __strscpy_sel(_1, _2, _3, NAME, ...) NAME
#define strscpy(...) __strscpy_sel(__VA_ARGS__, __strscpy_3, __strscpy_2)(__VA_ARGS__)
#define __strscpy_pad_2(d, s) lkpi_strscpy_pad(d, s, sizeof(d))
#define __strscpy_pad_3(d, s, n) lkpi_strscpy_pad(d, s, n)
#define strscpy_pad(...) __strscpy_sel(__VA_ARGS__, __strscpy_pad_3, __strscpy_pad_2)(__VA_ARGS__)
#define sized_strscpy lkpi_strscpy

static inline char *strstrip(char *str) { return strim(str); }
#define __sysfs_match_string(a, n, s) match_string(a, n, s)
#define sysfs_match_string(_a, _s) match_string(_a, ARRAY_SIZE(_a), _s)

static inline bool str_has_prefix_bool(const char *str, const char *prefix)
{
	return strncmp(str, prefix, strlen(prefix)) == 0;
}
static inline size_t str_has_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) == 0 ? len : 0;
}
static inline bool strstarts(const char *str, const char *prefix)
{
	return strncmp(str, prefix, strlen(prefix)) == 0;
}
static inline bool strends(const char *str, const char *suffix)
{
	size_t n = strlen(str), m = strlen(suffix);
	return n >= m && !strcmp(str + n - m, suffix);
}
static inline void *memset32(u32 *s, u32 v, size_t count)
{
	for (size_t i = 0; i < count; i++) s[i] = v;
	return s;
}
static inline void *memset64(u64 *s, u64 v, size_t count)
{
	for (size_t i = 0; i < count; i++) s[i] = v;
	return s;
}
static inline void *memset_p(void **p, void *v, size_t n)
{
	return memset64((u64 *)p, (uintptr_t)v, n);
}
static inline void memcpy_and_pad(void *dest, size_t dest_len, const void *src,
				  size_t count, int pad)
{
	if (dest_len > count) {
		memcpy(dest, src, count);
		memset((char *)dest + count, pad, dest_len - count);
	} else {
		memcpy(dest, src, dest_len);
	}
}
#define memzero_explicit(s, n) do { memset(s, 0, n); barrier_data(s); } while (0)
#define memset_startat(obj, v, member) \
	({ u8 *__ptr = (u8 *)(obj); typeof(v) __val = (v); \
	   memset(__ptr + offsetof(typeof(*(obj)), member), __val, \
		  sizeof(*(obj)) - offsetof(typeof(*(obj)), member)); })
#define memset_after(obj, v, member) \
	({ u8 *__ptr = (u8 *)(obj); typeof(v) __val = (v); \
	   memset(__ptr + offsetofend(typeof(*(obj)), member), __val, \
		  sizeof(*(obj)) - offsetofend(typeof(*(obj)), member)); })
#define strtomem(dest, src) strncpy(dest, src, sizeof(dest))
#define strtomem_pad(dest, src, pad) \
	memcpy_and_pad(dest, sizeof(dest), src, strnlen(src, sizeof(dest)), pad)
static inline void *kmemdup_array(const void *src, size_t count, size_t size, gfp_t gfp)
{
	return kmemdup(src, count * size, gfp);
}

#endif
