/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_PRANDOM_H
#define _LKPI_LINUX_PRANDOM_H
#include <linux/types.h>
#include <linux/string.h>
/* taus113 de Linux (lib/random32.c), mismas constantes */
struct rnd_state { u32 s1, s2, s3, s4; };
u32 lkpi_prandom_u32(void);
static inline u32 prandom_u32(void) { return lkpi_prandom_u32(); }
void lkpi_get_random_bytes(void *buf, size_t len);
static inline void prandom_bytes(void *buf, size_t nbytes) { lkpi_get_random_bytes(buf, nbytes); }
static inline u32 prandom_u32_state(struct rnd_state *state)
{
#define TAUSWORTHE(s, a, b, c, d) ((s & c) << d) ^ (((s << a) ^ s) >> b)
	state->s1 = TAUSWORTHE(state->s1,  6U, 13U, 4294967294U, 18U);
	state->s2 = TAUSWORTHE(state->s2,  2U, 27U, 4294967288U,  2U);
	state->s3 = TAUSWORTHE(state->s3, 13U, 21U, 4294967280U,  7U);
	state->s4 = TAUSWORTHE(state->s4,  3U, 12U, 4294967168U, 13U);
	return state->s1 ^ state->s2 ^ state->s3 ^ state->s4;
}
static inline u32 __seed(u32 x, u32 m) { return x < m ? x + m : x; }
static inline void prandom_seed_state(struct rnd_state *state, u64 seed)
{
	u32 i = ((seed >> 32) ^ (seed << 10) ^ seed) & 0xffffffffUL;

	state->s1 = __seed(i,   2U);
	state->s2 = __seed(i,   8U);
	state->s3 = __seed(i,  16U);
	state->s4 = __seed(i, 128U);
	for (i = 0; i < 10; i++)
		prandom_u32_state(state);
}
static inline void prandom_bytes_state(struct rnd_state *state, void *buf, size_t bytes)
{
	u8 *ptr = buf;

	while (bytes >= sizeof(u32)) {
		u32 v = prandom_u32_state(state);

		memcpy(ptr, &v, sizeof(v));
		ptr += sizeof(u32);
		bytes -= sizeof(u32);
	}
	if (bytes > 0) {
		u32 rem = prandom_u32_state(state);

		memcpy(ptr, &rem, bytes);
	}
}
static inline u32 prandom_u32_max(u32 ep_ro) { return (u32)(((u64)prandom_u32() * ep_ro) >> 32); }
#endif
