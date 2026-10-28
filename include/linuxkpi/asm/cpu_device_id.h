/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ASM_CPU_DEVICE_ID_H
#define _LKPI_ASM_CPU_DEVICE_ID_H
#include <linux/types.h>
#include <linux/mod_devicetable.h>
/* Solo coincidencia exacta de vendor/familia/modelo (VFM) con la CPU de arranque. */
struct x86_cpu_id { u32 vfm; kernel_ulong_t driver_data; };
#define VFM_MODEL(vfm)	((vfm) & 0xff)
#define VFM_FAMILY(vfm)	(((vfm) >> 8) & 0xff)
#define VFM_MAKE(vendor, family, model) (((vendor) << 16) | ((family) << 8) | (model))
#define X86_MATCH_VFM(vfm, data) { .vfm = (vfm), .driver_data = (kernel_ulong_t)(data) }
const struct x86_cpu_id *lkpi_x86_match_cpu(const struct x86_cpu_id *match);
#define x86_match_cpu(m) lkpi_x86_match_cpu(m)
#endif
