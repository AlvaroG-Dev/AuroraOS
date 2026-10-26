/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_TIME64_H
#define _LKPI_LINUX_TIME64_H

#include <linux/types.h>
#include <linux/math.h>

#define MSEC_PER_SEC 1000L
#define USEC_PER_MSEC 1000L
#define NSEC_PER_USEC 1000L
#define NSEC_PER_MSEC 1000000L
#define USEC_PER_SEC 1000000L
#define NSEC_PER_SEC 1000000000L
#define PSEC_PER_SEC 1000000000000LL
#define FSEC_PER_SEC 1000000000000000LL
#define TIME64_MAX ((s64)~((u64)1 << 63))
#define KTIME_MAX ((s64)~((u64)1 << 63))
#define KTIME_SEC_MAX (KTIME_MAX / NSEC_PER_SEC)

struct timespec64 {
	time64_t tv_sec;
	long tv_nsec;
};

static inline s64 timespec64_to_ns(const struct timespec64 *ts)
{
	return ((s64)ts->tv_sec * NSEC_PER_SEC) + ts->tv_nsec;
}
static inline struct timespec64 ns_to_timespec64(s64 nsec)
{
	struct timespec64 ts = { 0, 0 };
	if (nsec) {
		ts.tv_sec = nsec / NSEC_PER_SEC;
		ts.tv_nsec = nsec % NSEC_PER_SEC;
		if (ts.tv_nsec < 0) {
			ts.tv_sec--;
			ts.tv_nsec += NSEC_PER_SEC;
		}
	}
	return ts;
}
static inline struct timespec64 timespec64_sub(struct timespec64 a, struct timespec64 b)
{
	return ns_to_timespec64(timespec64_to_ns(&a) - timespec64_to_ns(&b));
}
static inline struct timespec64 timespec64_add(struct timespec64 a, struct timespec64 b)
{
	return ns_to_timespec64(timespec64_to_ns(&a) + timespec64_to_ns(&b));
}
static inline bool timespec64_valid(const struct timespec64 *ts)
{
	return ts->tv_sec >= 0 && (unsigned long)ts->tv_nsec < NSEC_PER_SEC;
}

#endif
