/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_KDEV_T_H
#define _LKPI_LINUX_KDEV_T_H

#include <linux/types.h>

/* dev_t interno de Linux: 12 bits de major y 20 de minor. */
#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)
#define MAJOR(dev) ((unsigned int)((dev) >> MINORBITS))
#define MINOR(dev) ((unsigned int)((dev) & MINORMASK))
#define MKDEV(ma, mi) (((ma) << MINORBITS) | (mi))
static inline bool old_valid_dev(dev_t dev)
{
	return MAJOR(dev) < 256 && MINOR(dev) < 256;
}

static inline u16 old_encode_dev(dev_t dev)
{
	return (MAJOR(dev) << 8) | MINOR(dev);
}

static inline u32 new_encode_dev(dev_t dev)
{
	unsigned int major = MAJOR(dev), minor = MINOR(dev);

	return (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
}

#define format_dev_t(buffer, dev) \
	({ sprintf(buffer, "%u:%u", MAJOR(dev), MINOR(dev)); buffer; })

#endif
