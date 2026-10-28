// SPDX-License-Identifier: GPL-2.0-only
/*
 * lkpi_i915.kur: lo que i915 espera de x86/kernel y del resto de Linux y que la
 * capa no tiene como biblioteca (Plan_LinuxKPI.md §11, L7):
 *  - remap_io_mapping()/remap_io_sg(): sustituyen a i915_mm.c (que usa
 *    apply_to_page_range y PTE directas) con lkpi_remap_pfn_range();
 *  - intel_gmch_*: la GTT de los chipsets Gen2-5 (drivers/char/agp/intel-gtt.c)
 *    no esta portada; intel_gmch_probe() falla y i915 no engancha esos equipos;
 *  - intel_graphics_stolen_res: la memoria stolen de Gen6+ se lee del registro BSM;
 *  - ips_link_to_i915_driver(): no hay driver ips (Ironlake), no hace nada.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/io-mapping.h>
#include <linux/scatterlist.h>
#include <linux/utsname.h>
#include <platform/x86/intel_ips.h>
#include <drm/intel/intel-gtt.h>

#include "i915_mm.h"

struct new_utsname lkpi_init_utsname = {
	.sysname = "Linux",
	.nodename = "aurora",
	.release = "6.18.59-aurora",
	.version = "#1",
	.machine = "x86_64",
};
EXPORT_SYMBOL(lkpi_init_utsname);

/* ------------------------------------------------------------ mapeos */

int remap_io_mapping(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size,
		     struct io_mapping *iomap)
{
	pgprot_t prot = pgprot_writecombine(vma->vm_page_prot);

	return lkpi_remap_pfn_range(vma, addr, pfn, size, prot);
}

int remap_io_sg(struct vm_area_struct *vma, unsigned long addr, unsigned long size, struct scatterlist *sgl,
		unsigned long offset, resource_size_t iobase)
{
	const bool use_dma = iobase != (resource_size_t)-1;
	struct scatterlist *sg = sgl;
	unsigned long done = 0;
	int err;

	/* saltar `offset` paginas */
	while (sg && offset >= sg->length >> PAGE_SHIFT) {
		offset -= sg->length >> PAGE_SHIFT;
		sg = sg_next(sg);
	}
	if (!sg)
		return -EINVAL;

	while (done < size) {
		unsigned long run, pfn;

		if (!sg)
			return -EINVAL;
		run = min_t(unsigned long, (sg->length - (offset << PAGE_SHIFT)), size - done);
		if (use_dma)
			pfn = (iobase + sg_dma_address(sg) + (offset << PAGE_SHIFT)) >> PAGE_SHIFT;
		else
			pfn = page_to_pfn(sg_page(sg)) + offset;
		err = lkpi_remap_pfn_range(vma, addr + done, pfn, run, vma->vm_page_prot);
		if (err) {
			lkpi_zap_vma_ptes(vma, addr, done);
			return err;
		}
		done += run;
		offset = 0;
		sg = sg_next(sg);
	}
	return 0;
}

/* ------------------------------------------- GMCH GTT (Gen2-5): no portada */

int intel_gmch_probe(struct pci_dev *bridge_pdev, struct pci_dev *gpu_pdev, struct agp_bridge_data *bridge)
{
	return 0;	/* 0 = fallo, como en Linux */
}
void intel_gmch_gtt_get(u64 *gtt_total, phys_addr_t *mappable_base, resource_size_t *mappable_end) {}
void intel_gmch_gtt_flush(void) {}
void intel_gmch_gtt_insert_page(dma_addr_t addr, unsigned int pg, unsigned int flags) {}
void intel_gmch_gtt_insert_sg_entries(struct sg_table *st, unsigned int pg_start, unsigned int flags) {}
void intel_gmch_gtt_clear_range(unsigned int first_entry, unsigned int num_entries) {}
void intel_gmch_remove(void) {}
bool intel_gmch_enable_gtt(void) { return false; }
dma_addr_t intel_gmch_gtt_read_entry(unsigned int pg, bool *is_present, bool *is_local)
{
	*is_present = false;
	*is_local = false;
	return 0;
}

struct resource intel_graphics_stolen_res = {
	.name = "Graphics Stolen Memory",
	.flags = IORESOURCE_MEM,
};
EXPORT_SYMBOL(intel_graphics_stolen_res);

/*
 * Memoria stolen (Gen6-Gen9): en Linux la rellena arch/x86/kernel/early-quirks.c
 * al arrancar; aqui se lee de la configuracion PCI del 00:02.0 antes de crear
 * la region (parche en i915_gem_stolen.c). Sin ella i915 cree que no hay
 * stolen, no encuentra el framebuffer del firmware y apaga el plano (pantalla
 * en negro). BSM (0x5c) da la base; GMCH_CTRL (0x50) el tamano. Gen11+ y
 * Cherryview leen otros registros y quedan sin stolen (se avisa).
 */
#define LKPI_INTEL_GMCH_CTRL	0x50
#define LKPI_INTEL_BSM		0x5c

void lkpi_intel_stolen_detect(struct pci_dev *pdev, int ver)
{
	u16 ctrl;
	u32 bsm;
	u64 size, base;

	if (resource_size(&intel_graphics_stolen_res) > 1)
		return;
	if (ver < 6 || ver > 9) {
		pr_warn("i915: memoria stolen de Gen%d sin soporte en la capa\n", ver);
		return;
	}
	pci_read_config_word(pdev, LKPI_INTEL_GMCH_CTRL, &ctrl);
	pci_read_config_dword(pdev, LKPI_INTEL_BSM, &bsm);
	base = bsm & ~((1u << 20) - 1);
	if (ver < 8) {
		size = (u64)((ctrl >> 3) & 0x1f) << 25;
	} else {
		u32 gms = (ctrl >> 8) & 0xff;

		if (ver == 9 && gms >= 0xf0)
			size = (u64)(gms - 0xf0 + 1) << 22;
		else
			size = (u64)gms << 25;
	}
	if (!base || !size) {
		pr_warn("i915: sin memoria stolen (BSM=%08x GMCH_CTRL=%04x)\n", bsm, ctrl);
		return;
	}
	intel_graphics_stolen_res.start = base;
	intel_graphics_stolen_res.end = base + size - 1;
	pr_info("i915: memoria stolen %llu MiB en %#llx\n", (unsigned long long)(size >> 20),
		(unsigned long long)base);
}
EXPORT_SYMBOL(lkpi_intel_stolen_detect);

void ips_link_to_i915_driver(void) {}

/*
 * Diagnostico (`lkpi.i915_diag=1`): tras el probe vuelca el estado real de pipe
 * A, plano primario, DDI, panel y retroiluminacion, y las primeras PTE de la
 * GGTT, para saber por que la pantalla queda en negro (plano apagado, pipe
 * parado, panel sin energia o backlight a cero). Solo lee registros.
 */
int lkpi_cmdline_get(const char *key, char *out, size_t outlen) __asm__("cmdline_get");

void lkpi_i915_diag(void)
{
	static const struct { const char *n; u32 off; } regs[] = {
		{ "PIPECONF_A", 0x70008 }, { "PIPECONF_B", 0x71008 }, { "PIPECONF_C", 0x72008 },
		{ "HTOTAL_A", 0x60000 }, { "HTOTAL_B", 0x61000 },
		{ "DSPCNTR_A", 0x70180 }, { "DSPSTRIDE_A", 0x70188 }, { "DSPSURF_A", 0x7019c },
		{ "DSPCNTR_B", 0x71180 }, { "DSPSURF_B", 0x7119c },
		{ "TRANS_DDI_FUNC_A", 0x60400 }, { "TRANS_DDI_FUNC_EDP", 0x6f400 },
		{ "DDI_BUF_CTL_A", 0x64000 }, { "DDI_BUF_CTL_B", 0x64100 },
		{ "DDI_BUF_CTL_C", 0x64200 }, { "DDI_BUF_CTL_D", 0x64300 },
		{ "PP_STATUS", 0xc7200 }, { "PP_CONTROL", 0xc7204 },
		{ "BLC_PWM_CPU_CTL2", 0x48250 }, { "BLC_PWM_CPU_CTL", 0x48254 },
		{ "BLC_PWM_PCH_CTL1", 0xc8250 }, { "BLC_PWM_PCH_CTL2", 0xc8254 },
		{ "HSW_PWR_WELL_CTL1", 0x45400 }, { "HSW_PWR_WELL_CTL2", 0x45404 },
		{ "GFX_FLSH_CNTL", 0x101008 },
	};
	struct pci_dev *pdev = NULL;
	void __iomem *mmio;
	char opt[8];
	unsigned int i;

	if (!lkpi_cmdline_get("lkpi.i915_diag", opt, sizeof(opt)) || opt[0] == '0')
		return;
	while ((pdev = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, pdev))) {
		if (pdev->vendor != PCI_VENDOR_ID_INTEL || !pdev->driver)
			continue;
		mmio = ioremap(pci_resource_start(pdev, 0), 4 << 20);
		if (!mmio)
			continue;
		pr_info("i915-diag: %s BAR0=%#llx\n", pci_name(pdev),
			(unsigned long long)pci_resource_start(pdev, 0));
		for (i = 0; i < ARRAY_SIZE(regs); i++)
			pr_info("i915-diag: %-18s [%#07x] = %#010x\n", regs[i].n, regs[i].off,
				readl(mmio + regs[i].off));
		{
			u32 surf = readl(mmio + 0x7019c);
			u32 pte0 = readl(mmio + (2 << 20));
			u32 pteS = readl(mmio + (2 << 20) + (surf >> 12) * 4);

			pr_info("i915-diag: GGTT[0]=%#x GGTT[surf]=%#x (surf=%#x)\n", pte0, pteS, surf);
		}
		iounmap(mmio);
	}
}

MODULE_LICENSE("GPL");
