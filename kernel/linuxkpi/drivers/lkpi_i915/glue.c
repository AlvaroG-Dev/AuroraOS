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

struct resource intel_graphics_stolen_res;
EXPORT_SYMBOL(intel_graphics_stolen_res);

void ips_link_to_i915_driver(void) {}

MODULE_LICENSE("GPL");
