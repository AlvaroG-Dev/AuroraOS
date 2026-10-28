/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_MFD_INTEL_SOC_PMIC_H
#define _LKPI_LINUX_MFD_INTEL_SOC_PMIC_H
#include <linux/types.h>
#include <linux/errno.h>
static inline int intel_soc_pmic_exec_mipi_pmic_seq_element(u16 i2c_address, u32 reg_address, u32 value, u32 mask)
{
	return -ENODEV;
}
#endif
