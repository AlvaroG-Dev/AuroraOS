/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_KERNEL_H
#define _LKPI_LINUX_KERNEL_H

#include <linux/stdarg.h>
#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/limits.h>
#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/container_of.h>
#include <linux/kstrtox.h>
#include <linux/log2.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/printk.h>
#include <linux/sprintf.h>
#include <linux/stddef.h>
#include <linux/stringify.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/bug.h>
#include <linux/byteorder/generic.h>
#include <asm/barrier.h>

#define STACK_MAGIC 0xdeadbeef
#define REPEAT_BYTE(x) ((~0ul / 0xff) * (x))
#define u64_to_user_ptr(x) ((void __user *)(uintptr_t)(x))
#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))
#define upper_16_bits(n) ((u16)((n) >> 16))
#define lower_16_bits(n) ((u16)((n) & 0xffff))
#include <linux/instruction_pointer.h>
#define TAINT_WARN 9
#define TAINT_MACHINE_CHECK 4
#define TAINT_USER 6
#define TAINT_CRAP 10
#define TAINT_OOT_MODULE 12
#define LOCKDEP_STILL_OK 0
#define LOCKDEP_NOW_UNRELIABLE 1
#define add_taint(flag, lockdep) do { } while (0)
#define might_sleep() do { } while (0)
#define might_sleep_if(cond) do { } while (0)
#define might_fault() do { } while (0)
#define might_resched() do { } while (0)
#define cant_sleep() do { } while (0)
#define non_block_start() do { } while (0)
#define non_block_end() do { } while (0)
#define sched_annotate_sleep() do { } while (0)
#define trace_printk(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#define tracing_off() do { } while (0)
#define dump_stack() lkpi_dump_stack()
void lkpi_dump_stack(void);
void __noreturn lkpi_panic(const char *fmt, ...) __printf(1, 2);
#define panic(fmt, ...) lkpi_panic(fmt, ##__VA_ARGS__)
#define hex_asc_lo(x) hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x) hex_asc[((x) & 0xf0) >> 4]
#define hex_asc_upper_lo(x) hex_asc_upper[((x) & 0x0f)]
#define hex_asc_upper_hi(x) hex_asc_upper[((x) & 0xf0) >> 4]
#define hex_asc "0123456789abcdef"
#define hex_asc_upper "0123456789ABCDEF"
static inline char *hex_byte_pack(char *buf, u8 byte)
{
	*buf++ = hex_asc_hi(byte);
	*buf++ = hex_asc_lo(byte);
	return buf;
}
static inline int hex_to_bin(unsigned char ch)
{
	unsigned char cu = ch & 0xdf;
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (cu >= 'A' && cu <= 'F')
		return cu - 'A' + 10;
	return -1;
}
int lkpi_hex2bin(u8 *dst, const char *src, size_t count);
char *lkpi_bin2hex(char *dst, const void *src, size_t count);
#define hex2bin lkpi_hex2bin
#define bin2hex lkpi_bin2hex

enum system_states { SYSTEM_BOOTING, SYSTEM_SCHEDULING, SYSTEM_FREEING_INITMEM,
		     SYSTEM_RUNNING, SYSTEM_HALT, SYSTEM_POWER_OFF, SYSTEM_RESTART,
		     SYSTEM_SUSPEND };
#define system_state SYSTEM_RUNNING
#define oops_in_progress 0


#define U64_C(x) x##ULL
#define U32_C(x) x##U
#endif
