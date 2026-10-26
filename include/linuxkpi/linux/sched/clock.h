/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SCHED_CLOCK_H
#define _LKPI_LINUX_SCHED_CLOCK_H

#include <linux/ktime.h>
#include <linux/smp.h>

#define sched_clock() lkpi_ktime_get_ns()
#define cpu_clock(cpu) lkpi_ktime_get_ns()
#define running_clock() lkpi_ktime_get_ns()

#endif
