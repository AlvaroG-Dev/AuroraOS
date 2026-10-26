/* SPDX-License-Identifier: GPL-2.0-only */
/* jiffies: el contador de ticks de Aurora (1 ms; HZ = 1000). */
#ifndef _LKPI_LINUX_JIFFIES_H
#define _LKPI_LINUX_JIFFIES_H

#include <linux/types.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/time64.h>

#define HZ CONFIG_HZ
#define USER_HZ 100
#define MAX_JIFFY_OFFSET ((LONG_MAX >> 1) - 1)
#define MAX_SCHEDULE_TIMEOUT LONG_MAX
#define INITIAL_JIFFIES 0UL

unsigned long lkpi_jiffies(void);
#define jiffies (lkpi_jiffies())
#define jiffies_64 ((u64)lkpi_jiffies())
static inline u64 get_jiffies_64(void) { return lkpi_jiffies(); }

#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#define time_in_range(a, b, c) (time_after_eq(a, b) && time_before_eq(a, c))
#define time_in_range_open(a, b, c) (time_after_eq(a, b) && time_before(a, c))
#define time_after64(a, b) ((s64)((b) - (a)) < 0)
#define time_before64(a, b) time_after64(b, a)
#define time_after_eq64(a, b) ((s64)((a) - (b)) >= 0)
#define time_before_eq64(a, b) time_after_eq64(b, a)
#define time_is_before_jiffies(a) time_after(jiffies, a)
#define time_is_after_jiffies(a) time_before(jiffies, a)
#define time_is_before_eq_jiffies(a) time_after_eq(jiffies, a)
#define time_is_after_eq_jiffies(a) time_before_eq(jiffies, a)

/* HZ = 1000: un jiffy es un milisegundo. */
static inline unsigned int jiffies_to_msecs(const unsigned long j) { return (unsigned int)j; }
static inline unsigned int jiffies_to_usecs(const unsigned long j) { return (unsigned int)(j * 1000); }
static inline u64 jiffies_to_nsecs(const unsigned long j) { return (u64)j * NSEC_PER_MSEC; }
static inline u64 jiffies64_to_nsecs(u64 j) { return j * NSEC_PER_MSEC; }
static inline u64 jiffies64_to_msecs(u64 j) { return j; }
static inline unsigned long msecs_to_jiffies(const unsigned int m)
{
	return m > MAX_JIFFY_OFFSET ? MAX_JIFFY_OFFSET : m;
}
static inline unsigned long usecs_to_jiffies(const unsigned int u)
{
	return DIV_ROUND_UP(u, 1000U);
}
static inline unsigned long nsecs_to_jiffies(u64 n) { return (unsigned long)DIV_ROUND_UP_ULL(n, NSEC_PER_MSEC); }
static inline u64 nsecs_to_jiffies64(u64 n) { return DIV_ROUND_UP_ULL(n, NSEC_PER_MSEC); }
static inline unsigned long timespec64_to_jiffies(const struct timespec64 *ts)
{
	return (unsigned long)(ts->tv_sec * HZ + DIV_ROUND_UP(ts->tv_nsec, NSEC_PER_MSEC));
}
static inline void jiffies_to_timespec64(const unsigned long j, struct timespec64 *ts)
{
	ts->tv_sec = j / HZ;
	ts->tv_nsec = (long)(j % HZ) * NSEC_PER_MSEC;
}
static inline clock_t jiffies_to_clock_t(unsigned long x) { return x / (HZ / USER_HZ); }
#define round_jiffies(j) (j)
#define round_jiffies_up(j) (j)
#define round_jiffies_relative(j) (j)
#define round_jiffies_up_relative(j) (j)

#endif
