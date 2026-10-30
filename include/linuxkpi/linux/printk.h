/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * printk y pr_*. Salen por el log del kernel de Aurora (serie) con el nivel
 * equivalente; KERN_DEBUG va a KLOG_DEBUG.
 */
#ifndef _LKPI_LINUX_PRINTK_H
#define _LKPI_LINUX_PRINTK_H

#include <linux/instruction_pointer.h>
#include <linux/compiler.h>
#include <linux/kern_levels.h>
#include <linux/stdarg.h>
#include <linux/types.h>

/* %pV: formato anidado (dev_printk y compania). */
struct va_format {
	const char *fmt;
	va_list *va;
};

__printf(1, 2) int lkpi_printk(const char *fmt, ...);
__printf(1, 0) int lkpi_vprintk(const char *fmt, va_list args);

#define printk(fmt, ...) lkpi_printk(fmt, ##__VA_ARGS__)
#define vprintk(fmt, args) lkpi_vprintk(fmt, args)
#define _printk printk

static inline __printf(1, 2) int no_printk(const char *fmt, ...)
{
	return 0;
}

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#define pr_emerg(fmt, ...) printk(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...) printk(KERN_ALERT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...) printk(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...) printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning pr_warn
#define pr_notice(fmt, ...) printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...) printk(KERN_CONT fmt, ##__VA_ARGS__)
#define pr_devel(fmt, ...) no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#if defined(DEBUG)
#define pr_debug(fmt, ...) printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...) no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#endif

#define printk_once(fmt, ...) \
	({ static bool __print_once; bool __ret_print_once = !__print_once; \
	   if (!__print_once) { __print_once = true; printk(fmt, ##__VA_ARGS__); } \
	   unlikely(__ret_print_once); })
#define pr_emerg_once(fmt, ...) printk_once(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert_once(fmt, ...) printk_once(KERN_ALERT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit_once(fmt, ...) printk_once(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err_once(fmt, ...) printk_once(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn_once(fmt, ...) printk_once(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice_once(fmt, ...) printk_once(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info_once(fmt, ...) printk_once(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug_once(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)

/* Sin limitacion de ritmo todavia (ver Plan_LinuxKPI.md): se imprime todo. */
#define printk_ratelimit() 1
#define pr_emerg_ratelimited pr_emerg
#define pr_alert_ratelimited pr_alert
#define pr_crit_ratelimited pr_crit
#define pr_err_ratelimited pr_err
#define pr_warn_ratelimited pr_warn
#define pr_notice_ratelimited pr_notice
#define pr_info_ratelimited pr_info
#define pr_debug_ratelimited pr_debug

#define printk_deferred printk
#define printk_deferred_once printk_once

enum { DUMP_PREFIX_NONE, DUMP_PREFIX_ADDRESS, DUMP_PREFIX_OFFSET };
void lkpi_print_hex_dump(const char *level, const char *prefix_str,
			 int prefix_type, int rowsize, int groupsize,
			 const void *buf, size_t len, bool ascii);
#define print_hex_dump(level, prefix, type, rowsize, groupsize, buf, len, ascii) \
	lkpi_print_hex_dump(level, prefix, type, rowsize, groupsize, buf, len, ascii)
#define print_hex_dump_bytes(prefix, type, buf, len) \
	print_hex_dump(KERN_DEBUG, prefix, type, 16, 1, buf, len, true)
#define print_hex_dump_debug(prefix, type, rs, gs, buf, len, ascii) \
	do { } while (0)


int lkpi_hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf,
			    size_t linebuflen, bool ascii);
#define hex_dump_to_buffer lkpi_hex_dump_to_buffer
#endif
