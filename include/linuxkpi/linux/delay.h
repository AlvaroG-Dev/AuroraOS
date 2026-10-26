/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Esperas. udelay/ndelay/mdelay son esperas activas sobre el TSC. msleep y
 * usleep_range duermen; como los plazos de Aurora tienen una resolucion de
 * ~8 ms, usleep_range() con un minimo por debajo de LKPI_USLEEP_SPIN_US hace
 * espera activa en lugar de dormir (llega a tiempo y no tarda 8 ms).
 */
#ifndef _LKPI_LINUX_DELAY_H
#define _LKPI_LINUX_DELAY_H

#include <linux/types.h>
#include <linux/math.h>
#include <linux/sched.h>

#define MAX_UDELAY_MS 5
#define LKPI_USLEEP_SPIN_US 2000

void lkpi_ndelay(unsigned long nsecs);
void lkpi_msleep(unsigned int msecs);
unsigned long lkpi_msleep_interruptible(unsigned int msecs);
void lkpi_usleep_range_state(unsigned long min, unsigned long max, unsigned int state);

#define ndelay(n) lkpi_ndelay(n)
#define udelay(n) lkpi_ndelay((unsigned long)(n) * 1000UL)
#define mdelay(n) lkpi_ndelay((unsigned long)(n) * 1000000UL)
#define msleep(ms) lkpi_msleep(ms)
#define msleep_interruptible(ms) lkpi_msleep_interruptible(ms)
#define ssleep(s) lkpi_msleep((s) * 1000)
#define usleep_range(min, max) lkpi_usleep_range_state(min, max, TASK_UNINTERRUPTIBLE)
#define usleep_idle_range(min, max) lkpi_usleep_range_state(min, max, TASK_IDLE)
#define usleep_range_state(min, max, state) lkpi_usleep_range_state(min, max, state)

/* Como en Linux: espera activa hasta 10 us, usleep_range hasta 20 ms y msleep despues. */
static inline void fsleep(unsigned long usecs)
{
	if (usecs <= 10)
		udelay(usecs);
	else if (usecs <= 20000)
		usleep_range(usecs, 2 * usecs);
	else
		msleep(DIV_ROUND_UP(usecs, 1000));
}

#endif
