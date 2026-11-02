/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_RANDOM_H
#define _LKPI_LINUX_RANDOM_H
#include <linux/types.h>
#include <linux/prandom.h>
void lkpi_get_random_bytes(void *buf, size_t len);
static inline void get_random_bytes(void *buf, size_t len) { lkpi_get_random_bytes(buf, len); }
static inline u32 get_random_u32(void) { u32 v; lkpi_get_random_bytes(&v, sizeof(v)); return v; }
static inline u64 get_random_u64(void) { u64 v; lkpi_get_random_bytes(&v, sizeof(v)); return v; }
static inline u16 get_random_u16(void) { u16 v; lkpi_get_random_bytes(&v, sizeof(v)); return v; }
static inline u8 get_random_u8(void) { u8 v; lkpi_get_random_bytes(&v, sizeof(v)); return v; }
static inline unsigned long get_random_long(void) { unsigned long v; lkpi_get_random_bytes(&v, sizeof(v)); return v; }
static inline u32 get_random_u32_below(u32 ceil)
{
	u32 v;

	if (!ceil)
		return get_random_u32();
	/* rechazo, como Linux: sin sesgo de modulo */
	for (u32 lim = -ceil % ceil;;) {
		v = get_random_u32();
		if (v >= lim)
			return v % ceil;
	}
}
static inline u32 get_random_u32_above(u32 floor) { return floor + 1 + get_random_u32_below(~floor); }
static inline u32 get_random_u32_inclusive(u32 floor, u32 ceil) { return floor + get_random_u32_below(ceil - floor + 1); }
static inline int wait_for_random_bytes(void) { return 0; }
#endif
