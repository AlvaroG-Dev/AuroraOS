#ifndef _LKPI_LINUX_TIMECOUNTER_H
#define _LKPI_LINUX_TIMECOUNTER_H
#include <linux/types.h>
#define CYCLECOUNTER_MASK(bits) (u64)((bits) < 64 ? ((1ULL<<(bits))-1) : -1)
struct cyclecounter { u64 (*read)(struct cyclecounter *cc); u64 mask; u32 mult; u32 shift; };
struct timecounter { struct cyclecounter *cc; u64 cycle_last; u64 nsec; u64 mask; u64 frac; };
static inline u64 cyclecounter_cyc2ns(const struct cyclecounter *cc, u64 cycles, u64 mask, u64 *frac)
{ u64 ns = cycles; ns = (ns * cc->mult) + *frac; *frac = ns & mask; return ns >> cc->shift; }
static inline void timecounter_adjtime(struct timecounter *tc, s64 delta) { tc->nsec += delta; }
static inline void timecounter_init(struct timecounter *tc, struct cyclecounter *cc, u64 start)
{ tc->cc = cc; tc->cycle_last = cc->read(cc); tc->nsec = start; tc->mask = (1ULL << cc->shift) - 1; tc->frac = 0; }
static inline u64 timecounter_read(struct timecounter *tc)
{
	u64 cycle_now = tc->cc->read(tc->cc), delta = (cycle_now - tc->cycle_last) & tc->cc->mask;
	tc->nsec += cyclecounter_cyc2ns(tc->cc, delta, tc->mask, &tc->frac);
	tc->cycle_last = cycle_now;
	return tc->nsec;
}
static inline u64 timecounter_cyc2time(const struct timecounter *tc, u64 cycle_tstamp)
{
	u64 delta = (cycle_tstamp - tc->cycle_last) & tc->cc->mask, frac = tc->frac;
	return tc->nsec + cyclecounter_cyc2ns(tc->cc, delta, tc->mask, &frac);
}
#endif
