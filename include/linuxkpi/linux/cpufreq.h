/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_CPUFREQ_H
#define _LKPI_LINUX_CPUFREQ_H
/* Sin cpufreq: cpufreq_cpu_get() devuelve NULL y i915 usa la frecuencia del TSC. */
#include <linux/types.h>
struct cpufreq_policy { unsigned int cpuinfo_max_freq; unsigned int max; unsigned int min; };
static inline struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu) { return NULL; }
static inline void cpufreq_cpu_put(struct cpufreq_policy *policy) {}
#endif
