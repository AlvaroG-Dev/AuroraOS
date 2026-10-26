/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SMP_H
#define _LKPI_LINUX_SMP_H

#include <linux/types.h>
#include <linux/preempt.h>
#include <linux/cpumask.h>

/* cpu_local_t.cpu_id de Aurora, en %gs:16 (kernel/cpu.h). */
static __always_inline int raw_smp_processor_id(void)
{
	int id;
	__asm__ __volatile__("movl %%gs:16, %0" : "=r"(id));
	return id;
}
#define smp_processor_id() raw_smp_processor_id()
#define __smp_processor_id() raw_smp_processor_id()
#define get_cpu() ({ preempt_disable(); smp_processor_id(); })
#define put_cpu() preempt_enable()

#endif
