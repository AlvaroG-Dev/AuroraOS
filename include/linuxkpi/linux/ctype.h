/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_CTYPE_H
#define _LKPI_LINUX_CTYPE_H

#include <linux/compiler.h>

static inline int isdigit(int c) { return '0' <= c && c <= '9'; }
static inline int isupper(int c) { return 'A' <= c && c <= 'Z'; }
static inline int islower(int c) { return 'a' <= c && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isxdigit(int c)
{
	return isdigit(c) || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F');
}
static inline int isspace(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
static inline int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
static inline int isprint(int c) { return c >= 32 && c < 127; }
static inline int isgraph(int c) { return c > 32 && c < 127; }
static inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }
static inline int isascii(int c) { return (unsigned int)c <= 127; }
static inline int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
static inline int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
static inline char _tolower(const char c) { return c | 0x20; }
static inline int isodigit(const char c) { return c >= '0' && c <= '7'; }

#endif
