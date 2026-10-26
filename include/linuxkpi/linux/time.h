/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_TIME_H
#define _LKPI_LINUX_TIME_H

#include <linux/time64.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
#define CLOCK_TAI 11
#endif

#endif
