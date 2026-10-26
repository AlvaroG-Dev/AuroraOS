// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reloj monotono (ktime) sobre el TSC y esperas (ndelay, msleep,
 * usleep_range).
 *
 * Al cargar la capa se toma un ancla (TSC, ns) en un cambio de tick; desde
 * ahi ns = ancla + ciclos * mult >> 32, con la frecuencia que calibro Aurora.
 * Un maximo global hace el valor monotono aunque los TSC de las CPU no esten
 * perfectamente alineados. Sin TSC calibrado se usa el tick (1 ms).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

static u64 lkpi_tsc_freq;	/* Hz; 0 = sin TSC */
static u64 lkpi_tsc0, lkpi_ns0;
static u64 lkpi_ns_mult;	/* ns por ciclo << 32 */
static u64 lkpi_cyc_mult;	/* ciclos por ns << 32 */
static u64 lkpi_last_ns;
static s64 lkpi_real_offset_ns;

static __always_inline u64 lkpi_rdtsc(void)
{
	u32 lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

static u64 lkpi_tick_ns(void)
{
	return aurora_sched_get_ticks() * NSEC_PER_MSEC;
}

u64 lkpi_ktime_get_ns(void)
{
	u64 ns, last;

	if (!lkpi_tsc_freq)
		return lkpi_tick_ns();
	ns = lkpi_ns0 + (u64)(((unsigned __int128)(lkpi_rdtsc() - lkpi_tsc0) * lkpi_ns_mult) >> 32);
	last = READ_ONCE(lkpi_last_ns);
	while (ns > last) {
		if (__atomic_compare_exchange_n(&lkpi_last_ns, &last, ns, false,
						__ATOMIC_RELAXED, __ATOMIC_RELAXED))
			return ns;
	}
	return last;
}
EXPORT_SYMBOL(lkpi_ktime_get_ns);

u64 lkpi_ktime_get_real_ns(void)
{
	return lkpi_ktime_get_ns() + lkpi_real_offset_ns;
}
EXPORT_SYMBOL(lkpi_ktime_get_real_ns);

void lkpi_time_init(void)
{
	u64 freq = aurora_klog_get_tsc_freq();
	u64 t, spins = 0;

	if (freq) {
		/* Ancla en el flanco de un tick (espera como mucho ~1 ms). */
		t = aurora_sched_get_ticks();
		while (aurora_sched_get_ticks() == t && ++spins < (1ULL << 28))
			cpu_relax();
		if (aurora_sched_get_ticks() != t) {
			lkpi_tsc0 = lkpi_rdtsc();
			lkpi_ns0 = lkpi_tick_ns();
			lkpi_ns_mult = (NSEC_PER_SEC << 32) / freq;
			lkpi_cyc_mult = (freq << 32) / NSEC_PER_SEC;
			lkpi_last_ns = lkpi_ns0;
			smp_wmb();
			lkpi_tsc_freq = freq;
		}
	}
	lkpi_real_offset_ns = (s64)aurora_kernel_realtime_ms() * NSEC_PER_MSEC -
			      (s64)lkpi_ktime_get_ns();
	if (lkpi_tsc_freq)
		pr_info("linuxkpi: ktime con TSC a %llu kHz\n", lkpi_tsc_freq / 1000);
	else
		pr_warn("linuxkpi: TSC sin calibrar, ktime con la resolucion del tick\n");
}

/* ------------------------------------------------------------ esperas */

void lkpi_ndelay(unsigned long nsecs)
{
	u64 end;

	if (!lkpi_tsc_freq) {
		/* Sin TSC: al menos un tick completo por cada ms pedido. */
		u64 t = aurora_sched_get_ticks() + DIV_ROUND_UP(nsecs, NSEC_PER_MSEC) + 1;
		while (aurora_sched_get_ticks() < t)
			cpu_relax();
		return;
	}
	end = lkpi_rdtsc() + (u64)(((unsigned __int128)nsecs * lkpi_cyc_mult) >> 32) + 1;
	while ((s64)(lkpi_rdtsc() - end) < 0)
		cpu_relax();
}
EXPORT_SYMBOL(lkpi_ndelay);

/* Duerme en `state` hasta que pasen al menos `j` jiffies. */
static long lkpi_sleep_jiffies(unsigned long j, unsigned int state)
{
	unsigned long end = jiffies + j;
	long left = (long)j;

	while (left > 0) {
		__set_current_state(state);
		schedule_timeout(left);
		left = (long)(end - jiffies);
		if ((state & TASK_INTERRUPTIBLE) && signal_pending(current))
			break;
	}
	return left > 0 ? left : 0;
}

void lkpi_msleep(unsigned int msecs)
{
	/* +1: el tick en curso ya esta empezado. */
	lkpi_sleep_jiffies(msecs_to_jiffies(msecs) + 1, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(lkpi_msleep);

unsigned long lkpi_msleep_interruptible(unsigned int msecs)
{
	return jiffies_to_msecs(lkpi_sleep_jiffies(msecs_to_jiffies(msecs) + 1,
						   TASK_INTERRUPTIBLE));
}
EXPORT_SYMBOL(lkpi_msleep_interruptible);

void lkpi_usleep_range_state(unsigned long min, unsigned long max, unsigned int state)
{
	(void)max;
	if (min < LKPI_USLEEP_SPIN_US) {
		lkpi_ndelay(min * NSEC_PER_USEC);
		return;
	}
	lkpi_sleep_jiffies(usecs_to_jiffies(min) + 1, state);
}
EXPORT_SYMBOL(lkpi_usleep_range_state);
