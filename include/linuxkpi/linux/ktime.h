/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ktime_t en nanosegundos. ktime_get() cuenta con el TSC (frecuencia que
 * calibra Aurora) desde un ancla tomada al cargar la capa: resolucion de
 * ns y monotono entre CPU. Sin TSC calibrado, la del tick (1 ms).
 */
#ifndef _LKPI_LINUX_KTIME_H
#define _LKPI_LINUX_KTIME_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/time64.h>
#include <linux/jiffies.h>

typedef s64 ktime_t;

u64 lkpi_ktime_get_ns(void);
u64 lkpi_ktime_get_real_ns(void);

static inline ktime_t ktime_set(const s64 secs, const unsigned long nsecs)
{
	if (unlikely(secs >= KTIME_SEC_MAX))
		return KTIME_MAX;
	return secs * NSEC_PER_SEC + (s64)nsecs;
}
#define ktime_sub(lhs, rhs) ((lhs) - (rhs))
#define ktime_add(lhs, rhs) ((lhs) + (rhs))
#define ktime_add_unsafe(lhs, rhs) ((u64)(lhs) + (rhs))
#define ktime_add_ns(kt, nsval) ((kt) + (nsval))
#define ktime_sub_ns(kt, nsval) ((kt) - (nsval))
static inline ktime_t timespec64_to_ktime(struct timespec64 ts) { return ktime_set(ts.tv_sec, ts.tv_nsec); }
#define ktime_to_timespec64(kt) ns_to_timespec64((kt))
#define ktime_to_ns(kt) ((s64)(kt))
#define ns_to_ktime(ns) ((ktime_t)(ns))
static inline int ktime_compare(const ktime_t a, const ktime_t b)
{
	return a < b ? -1 : (a > b ? 1 : 0);
}
static inline bool ktime_after(const ktime_t a, const ktime_t b) { return ktime_compare(a, b) > 0; }
static inline bool ktime_before(const ktime_t a, const ktime_t b) { return ktime_compare(a, b) < 0; }
static inline s64 ktime_to_us(const ktime_t kt) { return kt / NSEC_PER_USEC; }
static inline s64 ktime_to_ms(const ktime_t kt) { return kt / NSEC_PER_MSEC; }
static inline s64 ktime_us_delta(const ktime_t later, const ktime_t earlier) { return ktime_to_us(later - earlier); }
static inline s64 ktime_ms_delta(const ktime_t later, const ktime_t earlier) { return ktime_to_ms(later - earlier); }
static inline ktime_t ktime_add_us(const ktime_t kt, const u64 usec) { return kt + usec * NSEC_PER_USEC; }
static inline ktime_t ktime_add_ms(const ktime_t kt, const u64 msec) { return kt + msec * NSEC_PER_MSEC; }
static inline ktime_t ktime_sub_us(const ktime_t kt, const u64 usec) { return kt - usec * NSEC_PER_USEC; }
static inline ktime_t ktime_sub_ms(const ktime_t kt, const u64 msec) { return kt - msec * NSEC_PER_MSEC; }
static inline ktime_t us_to_ktime(u64 us) { return us * NSEC_PER_USEC; }
static inline ktime_t ms_to_ktime(u64 ms) { return ms * NSEC_PER_MSEC; }
static inline s64 ktime_divns(const ktime_t kt, s64 div) { return kt / div; }
static inline s64 ktime_get_seconds(void) { return lkpi_ktime_get_ns() / NSEC_PER_SEC; }

static inline ktime_t ktime_get(void) { return lkpi_ktime_get_ns(); }
static inline ktime_t ktime_get_raw(void) { return lkpi_ktime_get_ns(); }
static inline ktime_t ktime_get_boottime(void) { return lkpi_ktime_get_ns(); }
static inline ktime_t ktime_get_real(void) { return lkpi_ktime_get_real_ns(); }
static inline u64 ktime_get_ns(void) { return lkpi_ktime_get_ns(); }
static inline u64 ktime_get_raw_ns(void) { return lkpi_ktime_get_ns(); }
static inline u64 ktime_get_mono_fast_ns(void) { return lkpi_ktime_get_ns(); }
static inline u64 ktime_get_boottime_ns(void) { return lkpi_ktime_get_ns(); }
static inline u64 ktime_get_real_ns(void) { return lkpi_ktime_get_real_ns(); }
static inline time64_t ktime_get_real_seconds(void) { return lkpi_ktime_get_real_ns() / NSEC_PER_SEC; }
static inline void ktime_get_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(lkpi_ktime_get_ns()); }
static inline void ktime_get_real_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(lkpi_ktime_get_real_ns()); }
static inline void ktime_get_raw_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(lkpi_ktime_get_ns()); }
#define local_clock() lkpi_ktime_get_ns()

#endif
