/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ASM_TSC_H
#define _LKPI_ASM_TSC_H
#include <linux/types.h>
unsigned int lkpi_tsc_khz(void);
#define tsc_khz lkpi_tsc_khz()
#endif
