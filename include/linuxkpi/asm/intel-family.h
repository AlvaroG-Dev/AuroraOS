/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ASM_INTEL_FAMILY_H
#define _LKPI_ASM_INTEL_FAMILY_H
#include <asm/cpu_device_id.h>
/* Solo los modelos que nombra i915 (familia 6, vendor Intel = 0). */
#define INTEL_FAM6(model)		VFM_MAKE(0, 6, model)
#define INTEL_KABYLAKE_L		INTEL_FAM6(0x8E)
#define INTEL_KABYLAKE			INTEL_FAM6(0x9E)
#define INTEL_COMETLAKE			INTEL_FAM6(0xA5)
#define INTEL_ROCKETLAKE		INTEL_FAM6(0xA7)
#define INTEL_ALDERLAKE			INTEL_FAM6(0x97)
#define INTEL_ALDERLAKE_L		INTEL_FAM6(0x9A)
#define INTEL_RAPTORLAKE		INTEL_FAM6(0xB7)
#define INTEL_RAPTORLAKE_P		INTEL_FAM6(0xBA)
#define INTEL_RAPTORLAKE_S		INTEL_FAM6(0xBF)
#endif
