/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_PLATFORM_X86_INTEL_IPS_H
#define _LKPI_PLATFORM_X86_INTEL_IPS_H
/* Intel Intelligent Power Sharing (Ironlake): el driver ips no existe; i915 no lo enlaza. */
#include <linux/types.h>
void ips_link_to_i915_driver(void);
#endif
