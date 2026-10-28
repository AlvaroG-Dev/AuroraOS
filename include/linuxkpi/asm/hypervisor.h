/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ASM_HYPERVISOR_H
#define _LKPI_ASM_HYPERVISOR_H
#include <linux/types.h>
#include <asm/cpufeature.h>
enum x86_hypervisor_type { X86_HYPER_NATIVE = 0, X86_HYPER_VMWARE, X86_HYPER_MS_HYPERV,
			   X86_HYPER_XEN_PV, X86_HYPER_XEN_HVM, X86_HYPER_KVM, X86_HYPER_JAILHOUSE,
			   X86_HYPER_ACRN, X86_HYPER_BHYVE, X86_HYPER_QNX };
/* La capa solo distingue «hay hipervisor» (CPUID); el tipo no se necesita. */
static inline bool hypervisor_is_type(enum x86_hypervisor_type type)
{
	return type == X86_HYPER_NATIVE && !boot_cpu_has(X86_FEATURE_HYPERVISOR);
}
#endif
