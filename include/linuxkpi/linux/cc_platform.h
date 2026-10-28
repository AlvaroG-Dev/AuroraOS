/* SPDX-License-Identifier: GPL-2.0-only */
/* Atributos de plataforma confidencial: Aurora no corre en SEV/TDX. */
#ifndef _LKPI_LINUX_CC_PLATFORM_H
#define _LKPI_LINUX_CC_PLATFORM_H

#include <linux/types.h>

enum cc_attr {
	CC_ATTR_MEM_ENCRYPT,
	CC_ATTR_HOST_MEM_ENCRYPT,
	CC_ATTR_GUEST_MEM_ENCRYPT,
	CC_ATTR_GUEST_STATE_ENCRYPT,
	CC_ATTR_GUEST_UNROLL_STRING_IO,
	CC_ATTR_GUEST_SEV_SNP,
	CC_ATTR_HOTPLUG_DISABLED,
};

static inline bool cc_platform_has(enum cc_attr attr)
{
	return false;
}

#endif
