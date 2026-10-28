/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_RATELIMIT_H
#define _LKPI_LINUX_RATELIMIT_H

#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define DEFAULT_RATELIMIT_INTERVAL (5 * HZ)
#define DEFAULT_RATELIMIT_BURST 10
#define RATELIMIT_MSG_ON_RELEASE 1

struct ratelimit_state {
	raw_spinlock_t lock;
	int interval;
	int burst;
	int printed;
	int missed;
	unsigned long begin;
	unsigned long flags;
};

#define RATELIMIT_STATE_INIT(name, interval_init, burst_init) { \
		.lock = { 0 }, \
		.interval = interval_init, \
		.burst = burst_init, \
	}
#define RATELIMIT_STATE_INIT_DISABLED RATELIMIT_STATE_INIT(ratelimit_state, 0, DEFAULT_RATELIMIT_BURST)
#define DEFINE_RATELIMIT_STATE(name, interval_init, burst_init) \
	struct ratelimit_state name = RATELIMIT_STATE_INIT(name, interval_init, burst_init)

static inline void ratelimit_state_init(struct ratelimit_state *rs, int interval, int burst)
{
	memset(rs, 0, sizeof(*rs));
	raw_spin_lock_init(&rs->lock);
	rs->interval = interval;
	rs->burst = burst;
}
static inline void ratelimit_set_flags(struct ratelimit_state *rs, unsigned long flags)
{
	rs->flags = flags;
}

/* 1 si se puede emitir; como Linux: `burst` por `interval`, sin mensaje de omitidos */
static inline int ___ratelimit(struct ratelimit_state *rs, const char *func)
{
	unsigned long flags;
	int ret = 1;

	if (!rs->interval)
		return 1;
	raw_spin_lock_irqsave(&rs->lock, flags);
	if (!rs->begin || time_after(jiffies, rs->begin + rs->interval)) {
		rs->begin = jiffies;
		rs->printed = 0;
		rs->missed = 0;
	}
	if (rs->burst && rs->burst > rs->printed)
		rs->printed++;
	else {
		rs->missed++;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&rs->lock, flags);
	return ret;
}
#define __ratelimit(state) ___ratelimit(state, __func__)

#endif
