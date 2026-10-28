/* SPDX-License-Identifier: GPL-2.0-only */
/* Features de CPU: se consultan con CPUID la primera vez. */
#ifndef _LKPI_ASM_CPUFEATURE_H
#define _LKPI_ASM_CPUFEATURE_H

#include <linux/types.h>
#include <asm/processor.h>

#define X86_FEATURE_CLFLUSH	(0 * 32 + 19)	/* CPUID.1:EDX[19] */
#define X86_FEATURE_XMM4_1	(4 * 32 + 19)	/* CPUID.1:ECX[19] */
#define X86_FEATURE_HYPERVISOR	(4 * 32 + 31)	/* CPUID.1:ECX[31] */
#define X86_FEATURE_CLFLUSHOPT	(9 * 32 + 23)	/* CPUID.7:EBX[23] */

bool lkpi_cpu_has(unsigned int feature);
#define static_cpu_has(f)	lkpi_cpu_has(f)
#define boot_cpu_has(f)		lkpi_cpu_has(f)
#define cpu_feature_enabled(f)	lkpi_cpu_has(f)

void lkpi_wbinvd_on_all_cpus(void);
#define wbinvd_on_all_cpus() lkpi_wbinvd_on_all_cpus()

#include <asm/special_insns.h>

/* FPU/SSE en el kernel: la capa se compila sin SSE, asi que no se usa (static_cpu_has(XMM4_1) = no). */
static inline void kernel_fpu_begin(void) { }
static inline void kernel_fpu_end(void) { }

#endif
