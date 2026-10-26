/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_FIND_H
#define _LKPI_LINUX_FIND_H

#include <linux/types.h>

unsigned long lkpi_find_next_bit(const unsigned long *addr, unsigned long size,
				 unsigned long offset, unsigned long invert);
unsigned long lkpi_find_last_bit(const unsigned long *addr, unsigned long size);
unsigned long lkpi_find_nth_bit(const unsigned long *addr, unsigned long size,
				unsigned long n);

static inline unsigned long find_next_bit(const unsigned long *addr,
					  unsigned long size, unsigned long offset)
{
	return lkpi_find_next_bit(addr, size, offset, 0);
}
static inline unsigned long find_next_zero_bit(const unsigned long *addr,
					       unsigned long size, unsigned long offset)
{
	return lkpi_find_next_bit(addr, size, offset, ~0UL);
}
static inline unsigned long find_first_bit(const unsigned long *addr, unsigned long size)
{
	return lkpi_find_next_bit(addr, size, 0, 0);
}
static inline unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	return lkpi_find_next_bit(addr, size, 0, ~0UL);
}
static inline unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	return lkpi_find_last_bit(addr, size);
}
static inline unsigned long find_nth_bit(const unsigned long *addr, unsigned long size,
					 unsigned long n)
{
	return lkpi_find_nth_bit(addr, size, n);
}

#endif
