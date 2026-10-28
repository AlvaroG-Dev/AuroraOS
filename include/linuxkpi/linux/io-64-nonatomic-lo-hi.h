/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_IO_64_NONATOMIC_LO_HI_H
#define _LKPI_LINUX_IO_64_NONATOMIC_LO_HI_H
#include <linux/io.h>
#include <linux/types.h>
static inline u64 lo_hi_readq(const volatile void __iomem *addr)
{
	const volatile u32 __iomem *p = addr;
	u32 lo = readl(p), hi = readl(p + 1);

	return lo | ((u64)hi << 32);
}
static inline void lo_hi_writeq(u64 val, volatile void __iomem *addr)
{
	writel(val, addr);
	writel(val >> 32, addr + 4);
}
#ifndef readq
#define readq lo_hi_readq
#endif
#ifndef writeq
#define writeq lo_hi_writeq
#endif
#endif
