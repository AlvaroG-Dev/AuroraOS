// SPDX-License-Identifier: GPL-2.0-only
/* Features de la CPU de arranque (CPUID) y wbinvd, para drm_cache y compania. */
#include <linux/export.h>
#include <linux/kernel.h>
#include <asm/cpufeature.h>
#include <asm/cpu_device_id.h>
#include "../aurora.h"

struct cpuinfo_x86 lkpi_boot_cpu_data;
EXPORT_SYMBOL(lkpi_boot_cpu_data);

static u32 lkpi_cpuid_leaf1_ecx, lkpi_cpuid_leaf1_edx, lkpi_cpuid_leaf7_ebx;
static bool lkpi_cpu_probed;

static void lkpi_cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
	asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "0"(leaf), "2"(sub));
}

static void lkpi_cpu_probe(void)
{
	u32 a, b, c, d, max;
	bool intel;

	lkpi_cpuid(0, 0, &max, &b, &c, &d);
	/* "GenuineIntel": EBX="Genu" EDX="ineI" ECX="ntel" */
	intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e;
	lkpi_cpuid(1, 0, &a, &b, &lkpi_cpuid_leaf1_ecx, &lkpi_cpuid_leaf1_edx);
	lkpi_boot_cpu_data.x86 = ((a >> 8) & 0xf) == 0xf ? 0xf + ((a >> 20) & 0xff) : (a >> 8) & 0xf;
	lkpi_boot_cpu_data.x86_model = (a >> 4) & 0xf;
	if (lkpi_boot_cpu_data.x86 == 6 || lkpi_boot_cpu_data.x86 >= 0xf)
		lkpi_boot_cpu_data.x86_model |= ((a >> 16) & 0xf) << 4;
	lkpi_boot_cpu_data.x86_vendor_intel = intel;
	lkpi_boot_cpu_data.x86_clflush_size = ((b >> 8) & 0xff) * 8;
	lkpi_boot_cpu_data.x86_cache_alignment = lkpi_boot_cpu_data.x86_clflush_size ?: 64;
	if (max >= 7) {
		lkpi_cpuid(7, 0, &a, &lkpi_cpuid_leaf7_ebx, &c, &d);
	}
	lkpi_cpu_probed = true;
}

bool lkpi_cpu_has(unsigned int feature)
{
	if (!lkpi_cpu_probed)
		lkpi_cpu_probe();
	switch (feature) {
	case X86_FEATURE_CLFLUSH:
		return lkpi_cpuid_leaf1_edx & (1u << 19);
	case X86_FEATURE_XMM4_1:
		return false;	/* sin kernel_fpu: nunca se usa MOVNTDQA */
	case X86_FEATURE_HYPERVISOR:
		return lkpi_cpuid_leaf1_ecx & (1u << 31);
	case X86_FEATURE_CLFLUSHOPT:
		return lkpi_cpuid_leaf7_ebx & (1u << 23);
	}
	return false;
}
EXPORT_SYMBOL(lkpi_cpu_has);

/* Solo la CPU actual: suficiente para volcar caches antes de un DMA incoherente. */
void lkpi_wbinvd_on_all_cpus(void)
{
	asm volatile("wbinvd" ::: "memory");
}
EXPORT_SYMBOL(lkpi_wbinvd_on_all_cpus);

/* vfm = vendor<<16 | familia<<8 | modelo; vendor 0 = Intel. */
const struct x86_cpu_id *lkpi_x86_match_cpu(const struct x86_cpu_id *match)
{
	if (!lkpi_cpu_probed)
		lkpi_cpu_probe();
	for (; match->vfm; match++)
		if ((match->vfm >> 16) == 0 && lkpi_boot_cpu_data.x86_vendor_intel &&
		    ((match->vfm >> 8) & 0xff) == lkpi_boot_cpu_data.x86 &&
		    (match->vfm & 0xff) == lkpi_boot_cpu_data.x86_model)
			return match;
	return NULL;
}
EXPORT_SYMBOL(lkpi_x86_match_cpu);

unsigned int lkpi_tsc_khz(void)
{
	return aurora_klog_get_tsc_freq() / 1000;
}
EXPORT_SYMBOL(lkpi_tsc_khz);

void lkpi_cpu_init(void)
{
	lkpi_cpu_probe();
}
