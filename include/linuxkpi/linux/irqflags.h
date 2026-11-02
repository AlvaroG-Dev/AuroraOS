/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_IRQFLAGS_H
#define _LKPI_LINUX_IRQFLAGS_H

#include <linux/compiler.h>

static __always_inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;
	__asm__ __volatile__("pushfq; popq %0" : "=rm"(flags) : : "memory");
	return flags;
}
static __always_inline void arch_local_irq_disable(void) { __asm__ __volatile__("cli" ::: "memory"); }
static __always_inline void arch_local_irq_enable(void) { __asm__ __volatile__("sti" ::: "memory"); }
static __always_inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags = arch_local_save_flags();
	arch_local_irq_disable();
	return flags;
}
static __always_inline void arch_local_irq_restore(unsigned long flags)
{
	if (flags & (1UL << 9))
		arch_local_irq_enable();
}
#define local_irq_disable() arch_local_irq_disable()
#define local_irq_enable() arch_local_irq_enable()
#define local_irq_save(flags) do { (flags) = arch_local_irq_save(); } while (0)
#define local_irq_restore(flags) arch_local_irq_restore(flags)
#define local_save_flags(flags) do { (flags) = arch_local_save_flags(); } while (0)
#define irqs_disabled() (!(arch_local_save_flags() & (1UL << 9)))
#define irqs_disabled_flags(flags) (!((flags) & (1UL << 9)))
#define lockdep_assert_irqs_disabled() do { } while (0)
#define lockdep_assert_irqs_enabled() do { } while (0)

#define X86_EFLAGS_IF 0x00000200
#endif
