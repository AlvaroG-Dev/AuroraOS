// SPDX-License-Identifier: GPL-2.0-only
/*
 * Aleatorios de la capa: RDRAND si la CPU lo tiene; si no, un generador
 * xorshift sembrado con el TSC. No es criptografico (los drivers de GPU lo
 * usan para ids y pruebas, no para claves).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/random.h>
#include <linux/spinlock.h>

static u64 lkpi_rng_state;
static DEFINE_SPINLOCK(lkpi_rng_lock);

static u64 lkpi_rdtsc(void)
{
	u32 lo, hi;

	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

static bool lkpi_rdrand64(u64 *v)
{
	unsigned char ok;

	for (int i = 0; i < 10; i++) {
		asm volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
		if (ok)
			return true;
	}
	return false;
}

static u64 lkpi_xorshift(void)
{
	u64 x;

	spin_lock(&lkpi_rng_lock);
	x = lkpi_rng_state;
	if (!x)
		x = lkpi_rdtsc() * 0x9e3779b97f4a7c15ULL | 1;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	lkpi_rng_state = x;
	spin_unlock(&lkpi_rng_lock);
	return x * 0x2545f4914f6cdd1dULL;
}

void lkpi_get_random_bytes(void *buf, size_t len)
{
	u8 *p = buf;
	static int has_rdrand = -1;

	if (has_rdrand < 0) {
		u32 a = 1, b, c, d;

		asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
		has_rdrand = !!(c & (1u << 30));
	}
	while (len) {
		u64 v;
		size_t n = len < sizeof(v) ? len : sizeof(v);

		if (!(has_rdrand && lkpi_rdrand64(&v)))
			v = lkpi_xorshift();
		memcpy(p, &v, n);
		p += n;
		len -= n;
	}
}
EXPORT_SYMBOL(lkpi_get_random_bytes);

u32 lkpi_prandom_u32(void)
{
	return (u32)(lkpi_xorshift() >> 16);
}
EXPORT_SYMBOL(lkpi_prandom_u32);
