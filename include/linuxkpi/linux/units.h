/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_UNITS_H
#define _LKPI_LINUX_UNITS_H
#define HZ_PER_KHZ	1000UL
#define KHZ_PER_MHZ	1000UL
#define HZ_PER_MHZ	1000000UL
#define MILLIHZ_PER_HZ	1000UL
#define MILLI		1000UL
#define MICRO		1000000UL
#define NANO		1000000000UL
#define NSEC_PER_USEC	1000L
#define MILLIWATT_PER_WATT 1000UL
#define MICROWATT_PER_MILLIWATT 1000UL
#define MICROWATT_PER_WATT 1000000UL
#define ABSOLUTE_ZERO_MILLICELSIUS -273150
static inline long milli_kelvin_to_millicelsius(long t) { return t + ABSOLUTE_ZERO_MILLICELSIUS; }
static inline long millicelsius_to_milli_kelvin(long t) { return t - ABSOLUTE_ZERO_MILLICELSIUS; }
#endif
