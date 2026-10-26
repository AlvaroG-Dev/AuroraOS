/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hrtimer: plazos en ns (ktime_get(), TSC) que se disparan en el hilo
 * lkpi_softirqd. La precision real es la del despertar de ese hilo (el tick
 * de Aurora, 1 ms, igual que timer_list); nunca se disparan antes de tiempo.
 */
#ifndef _LKPI_LINUX_HRTIMER_H
#define _LKPI_LINUX_HRTIMER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/time.h>

enum hrtimer_mode {
	HRTIMER_MODE_ABS = 0x00,
	HRTIMER_MODE_REL = 0x01,
	HRTIMER_MODE_PINNED = 0x02,
	HRTIMER_MODE_SOFT = 0x04,
	HRTIMER_MODE_HARD = 0x08,
	HRTIMER_MODE_ABS_PINNED = HRTIMER_MODE_ABS | HRTIMER_MODE_PINNED,
	HRTIMER_MODE_REL_PINNED = HRTIMER_MODE_REL | HRTIMER_MODE_PINNED,
	HRTIMER_MODE_ABS_SOFT = HRTIMER_MODE_ABS | HRTIMER_MODE_SOFT,
	HRTIMER_MODE_REL_SOFT = HRTIMER_MODE_REL | HRTIMER_MODE_SOFT,
	HRTIMER_MODE_ABS_PINNED_SOFT = HRTIMER_MODE_ABS_PINNED | HRTIMER_MODE_SOFT,
	HRTIMER_MODE_REL_PINNED_SOFT = HRTIMER_MODE_REL_PINNED | HRTIMER_MODE_SOFT,
	HRTIMER_MODE_ABS_HARD = HRTIMER_MODE_ABS | HRTIMER_MODE_HARD,
	HRTIMER_MODE_REL_HARD = HRTIMER_MODE_REL | HRTIMER_MODE_HARD,
	HRTIMER_MODE_ABS_PINNED_HARD = HRTIMER_MODE_ABS_PINNED | HRTIMER_MODE_HARD,
	HRTIMER_MODE_REL_PINNED_HARD = HRTIMER_MODE_REL_PINNED | HRTIMER_MODE_HARD,
};

enum hrtimer_restart {
	HRTIMER_NORESTART,
	HRTIMER_RESTART,
};

struct hrtimer {
	struct list_head node;		/* node.next == NULL: no encolado */
	ktime_t _softexpires;
	ktime_t node_expires;
	enum hrtimer_restart (*function)(struct hrtimer *);
	clockid_t clockid;
	u8 state;
	u8 is_rel;
};

#define HRTIMER_STATE_INACTIVE 0x00
#define HRTIMER_STATE_ENQUEUED 0x01

void lkpi_hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*fn)(struct hrtimer *),
			clockid_t clock_id, enum hrtimer_mode mode);
void lkpi_hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim, u64 range_ns,
				 enum hrtimer_mode mode);
int lkpi_hrtimer_try_to_cancel(struct hrtimer *timer);
int lkpi_hrtimer_cancel(struct hrtimer *timer);
bool lkpi_hrtimer_active(const struct hrtimer *timer);
u64 lkpi_hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval);

#define hrtimer_setup(t, fn, clk, mode) lkpi_hrtimer_setup(t, fn, clk, mode)
#define hrtimer_setup_on_stack(t, fn, clk, mode) lkpi_hrtimer_setup(t, fn, clk, mode)
#define hrtimer_init(t, clk, mode) lkpi_hrtimer_setup(t, NULL, clk, mode)
#define hrtimer_init_on_stack(t, clk, mode) lkpi_hrtimer_setup(t, NULL, clk, mode)
#define destroy_hrtimer_on_stack(t) do { (void)(t); } while (0)
#define hrtimer_start_range_ns(t, tim, delta, mode) lkpi_hrtimer_start_range_ns(t, tim, delta, mode)
#define hrtimer_start(t, tim, mode) lkpi_hrtimer_start_range_ns(t, tim, 0, mode)
#define hrtimer_start_expires(t, mode) \
	lkpi_hrtimer_start_range_ns(t, (t)->node_expires, 0, HRTIMER_MODE_ABS)
#define hrtimer_try_to_cancel(t) lkpi_hrtimer_try_to_cancel(t)
#define hrtimer_cancel(t) lkpi_hrtimer_cancel(t)
#define hrtimer_active(t) lkpi_hrtimer_active(t)
#define hrtimer_forward(t, now, interval) lkpi_hrtimer_forward(t, now, interval)
#define hrtimer_forward_now(t, interval) lkpi_hrtimer_forward(t, ktime_get(), interval)
#define hrtimer_update_function(t, fn) do { (t)->function = (fn); } while (0)

static inline bool hrtimer_is_queued(const struct hrtimer *t)
{
	return READ_ONCE(t->state) & HRTIMER_STATE_ENQUEUED;
}
static inline bool hrtimer_callback_running(const struct hrtimer *t)
{
	return lkpi_hrtimer_active(t) && !hrtimer_is_queued(t);
}
static inline ktime_t hrtimer_get_expires(const struct hrtimer *t) { return t->node_expires; }
static inline ktime_t hrtimer_get_softexpires(const struct hrtimer *t) { return t->_softexpires; }
static inline s64 hrtimer_get_expires_ns(const struct hrtimer *t) { return t->node_expires; }
static inline void hrtimer_set_expires(struct hrtimer *t, ktime_t time)
{
	t->node_expires = time;
	t->_softexpires = time;
}
static inline void hrtimer_set_expires_range_ns(struct hrtimer *t, ktime_t time, u64 delta)
{
	t->_softexpires = time;
	t->node_expires = time + delta;
}
static inline void hrtimer_add_expires_ns(struct hrtimer *t, u64 ns)
{
	t->node_expires += ns;
	t->_softexpires += ns;
}
static inline void hrtimer_add_expires(struct hrtimer *t, ktime_t time)
{
	hrtimer_add_expires_ns(t, time);
}
static inline ktime_t hrtimer_expires_remaining(const struct hrtimer *t)
{
	return t->node_expires - ktime_get();
}
#define hrtimer_get_remaining(t) hrtimer_expires_remaining(t)
/* Funcion, no macro: un macro hrtimer_restart() romperia "enum hrtimer_restart (*f)(...)". */
static inline void hrtimer_restart(struct hrtimer *t)
{
	hrtimer_start_expires(t, HRTIMER_MODE_ABS);
}
static inline ktime_t hrtimer_cb_get_time(const struct hrtimer *t) { (void)t; return ktime_get(); }

#endif
