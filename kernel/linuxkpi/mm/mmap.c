// SPDX-License-Identifier: GPL-2.0-only
/*
 * mmap de los drivers (K8, L4): vm_area_struct sobre los VMA_DEV de Aurora.
 *
 * Aurora crea el VMA (con referencia al fd) y llama a ->mmap_dev; aqui se
 * crea el vm_area_struct, se llama a f_op->mmap y se enganchan las
 * operaciones de Aurora (fallo de pagina, open en fork/division, close). Las
 * PTE las pone Aurora (vma_dev_map_page): PTE_SPECIAL|PTE_SHARED_OBJ y la
 * cache de vm_page_prot. Las paginas con struct page que se mapean
 * (vm_insert_page, vmf->page) guardan una referencia por pagina hasta que
 * se quitan (zap, unmap_mapping_range) o el VMA desaparece, como en Linux.
 *
 * vm_start/vm_end/vm_pgoff se leen del VMA de Aurora antes de cada llamada
 * al driver: un munmap que solo recorta no avisa (tampoco en Linux).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "../aurora.h"
#include "../lkpi_internal.h"
#include "../fs/fs.h"

#define MMAP_PROT_READ 0x1
#define MMAP_PROT_WRITE 0x2
#define MMAP_PROT_EXEC 0x4
#define MMAP_MAP_SHARED 0x1

struct lkpi_vma {
	struct vm_area_struct vma;
	struct aurora_vma *avma;
	struct xarray pages;		/* direccion >> PAGE_SHIFT -> struct page con ref */
};

static inline struct lkpi_vma *to_lvma(struct vm_area_struct *vma)
{
	return container_of(vma, struct lkpi_vma, vma);
}

static void lkpi_vma_sync(struct lkpi_vma *l)
{
	l->vma.vm_start = l->avma->start;
	l->vma.vm_end = l->avma->end;
	l->vma.vm_pgoff = l->avma->file_offset >> PAGE_SHIFT;
}

static u32 lkpi_prot_cache(pgprot_t prot)
{
	return (u32)(pgprot_val(prot) & (AURORA_PTE_WRITETHRU | AURORA_PTE_NOCACHE));
}

/* Mapea `phys` en `addr`; si `page`, guarda su referencia (suelta la anterior). */
static int lkpi_vma_map(struct lkpi_vma *l, unsigned long addr, phys_addr_t phys,
			struct page *page, pgprot_t prot)
{
	struct page *old;
	int rc;

	l->avma->dev_cache = lkpi_prot_cache(prot);
	rc = aurora_vma_dev_map_page(l->avma, addr, phys);
	if (rc)
		return rc;
	if (page)
		get_page(page);
	old = page ? xa_store(&l->pages, addr >> PAGE_SHIFT, page, GFP_KERNEL)
		   : xa_erase(&l->pages, addr >> PAGE_SHIFT);
	if (old && !xa_is_err(old))
		put_page(old);
	return 0;
}

static void lkpi_vma_put_range(struct lkpi_vma *l, unsigned long start, unsigned long end)
{
	unsigned long idx;
	struct page *page;

	xa_for_each_range(&l->pages, idx, page, start >> PAGE_SHIFT, (end - 1) >> PAGE_SHIFT) {
		xa_erase(&l->pages, idx);
		put_page(page);
	}
}

/* ------------------------------------------------- operaciones de Aurora */

static int lkpi_avma_fault(struct aurora_vma *avma, u64 addr, int write)
{
	struct lkpi_vma *l = avma->dev_priv;
	struct vm_area_struct *vma = &l->vma;
	struct vm_fault vmf = {};
	vm_fault_t ret;

	lkpi_vma_sync(l);
	if (!vma->vm_ops || !vma->vm_ops->fault)
		return -EFAULT;
	vmf.vma = vma;
	vmf.address = addr;
	vmf.real_address = addr;
	vmf.pgoff = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);
	vmf.flags = FAULT_FLAG_USER | FAULT_FLAG_KILLABLE | (write ? FAULT_FLAG_WRITE : 0);
	vmf.gfp_mask = GFP_KERNEL;
	ret = vma->vm_ops->fault(&vmf);
	if (ret & (VM_FAULT_ERROR | VM_FAULT_RETRY))
		return -EFAULT;
	if (ret & VM_FAULT_NOPAGE)
		return 0;	/* el driver ya mapeo (vmf_insert_*) */
	if (!vmf.page)
		return -EFAULT;
	/* ->fault devuelve la pagina con una referencia: pasa al mapeo. */
	ret = lkpi_vma_map(l, addr, page_to_phys(vmf.page), vmf.page, vma->vm_page_prot);
	put_page(vmf.page);
	return ret ? -EFAULT : 0;
}

static int lkpi_avma_open(struct aurora_vma *nv, struct aurora_vma *old)
{
	struct lkpi_vma *o = old->dev_priv, *l;
	struct address_space *mapping;
	unsigned long idx;
	struct page *page;

	l = kzalloc(sizeof(*l), GFP_KERNEL);
	if (!l)
		return -ENOMEM;
	lkpi_vma_sync(o);
	memcpy(&l->vma, &o->vma, sizeof(l->vma));
	l->avma = nv;
	xa_init(&l->pages);
	nv->dev_priv = l;
	lkpi_vma_sync(l);
	if (nv->dev_proc != old->dev_proc) {
		/* fork: el hijo comparte las PTE; cada pagina, otra referencia */
		l->vma.vm_mm = NULL;	/* el mm del hijo aun no existe en la capa */
		xa_for_each(&o->pages, idx, page) {
			get_page(page);
			xa_store(&l->pages, idx, page, GFP_KERNEL);
		}
	} else {
		/* division: las paginas del trozo nuevo pasan a el */
		xa_for_each_range(&o->pages, idx, page, l->vma.vm_start >> PAGE_SHIFT,
				  (l->vma.vm_end - 1) >> PAGE_SHIFT) {
			xa_erase(&o->pages, idx);
			xa_store(&l->pages, idx, page, GFP_KERNEL);
		}
	}
	mapping = l->vma.vm_file ? l->vma.vm_file->f_mapping : NULL;
	if (mapping) {
		mutex_lock(&mapping->i_mmap_mutex);
		list_add(&l->vma.__lkpi_mapping, &mapping->i_mmap);
		mutex_unlock(&mapping->i_mmap_mutex);
	} else {
		INIT_LIST_HEAD(&l->vma.__lkpi_mapping);
	}
	if (l->vma.vm_ops && l->vma.vm_ops->open)
		l->vma.vm_ops->open(&l->vma);
	return 0;
}

static void lkpi_avma_close(struct aurora_vma *avma)
{
	struct lkpi_vma *l = avma->dev_priv;
	struct address_space *mapping = l->vma.vm_file ? l->vma.vm_file->f_mapping : NULL;
	unsigned long idx;
	struct page *page;

	lkpi_vma_sync(l);
	if (l->vma.vm_ops && l->vma.vm_ops->close)
		l->vma.vm_ops->close(&l->vma);
	if (mapping) {
		mutex_lock(&mapping->i_mmap_mutex);
		list_del(&l->vma.__lkpi_mapping);
		mutex_unlock(&mapping->i_mmap_mutex);
	}
	xa_for_each(&l->pages, idx, page)
		put_page(page);
	xa_destroy(&l->pages);
	avma->dev_priv = NULL;
	kfree(l);
}

static const struct aurora_vma_dev_ops lkpi_avma_ops = {
	.fault = lkpi_avma_fault,
	.open = lkpi_avma_open,
	.close = lkpi_avma_close,
};

int lkpi_file_mmap(struct file *file, struct aurora_process *proc, struct aurora_vma *avma,
		   u64 prot, u64 flags)
{
	struct lkpi_vma *l;
	vm_flags_t vf = VM_MAYREAD | VM_MAYEXEC;
	int rc;

	if (!file->f_op->mmap)
		return -ENODEV;
	if ((prot & MMAP_PROT_READ) && !(file->f_mode & FMODE_READ))
		return -EACCES;
	if ((flags & MMAP_MAP_SHARED) && (prot & MMAP_PROT_WRITE) && !(file->f_mode & FMODE_WRITE))
		return -EACCES;
	if (prot & MMAP_PROT_READ)
		vf |= VM_READ;
	if (prot & MMAP_PROT_WRITE)
		vf |= VM_WRITE;
	if (prot & MMAP_PROT_EXEC)
		vf |= VM_EXEC;
	if (file->f_mode & FMODE_WRITE)
		vf |= VM_MAYWRITE;
	if (flags & MMAP_MAP_SHARED)
		vf |= VM_SHARED | VM_MAYSHARE;

	l = kzalloc(sizeof(*l), GFP_KERNEL);
	if (!l)
		return -ENOMEM;
	l->avma = avma;
	xa_init(&l->pages);
	lkpi_vma_sync(l);
	vm_flags_init(&l->vma, vf);
	l->vma.vm_page_prot = vm_get_page_prot(vf);
	l->vma.vm_file = file;
	l->vma.vm_mm = current->mm;
	l->vma.__lkpi_avma = avma;
	avma->dev_priv = l;
	rc = file->f_op->mmap(file, &l->vma);
	if (rc) {
		unsigned long idx;
		struct page *page;

		xa_for_each(&l->pages, idx, page)
			put_page(page);
		xa_destroy(&l->pages);
		avma->dev_priv = NULL;
		kfree(l);
		return lkpi_errno_user(rc);
	}
	/* El driver pudo cambiar flags y cache: valen para los fallos. */
	avma->dev_cache = lkpi_prot_cache(l->vma.vm_page_prot);
	if (l->vma.vm_flags & (VM_DONTCOPY | VM_WIPEONFORK))
		avma->dev_flags |= AURORA_VMA_DEV_DONTCOPY;
	mutex_lock(&file->f_mapping->i_mmap_mutex);
	list_add(&l->vma.__lkpi_mapping, &file->f_mapping->i_mmap);
	mutex_unlock(&file->f_mapping->i_mmap_mutex);
	avma->dev_ops = &lkpi_avma_ops;
	return 0;
}

/* ------------------------------------------------------- API de Linux */

int lkpi_remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn,
			 unsigned long size, pgprot_t prot)
{
	struct lkpi_vma *l = to_lvma(vma);
	unsigned long end = addr + PAGE_ALIGN(size);
	int rc;

	if (addr < vma->vm_start || end > vma->vm_end || (addr & ~PAGE_MASK))
		return -EINVAL;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	for (; addr < end; addr += PAGE_SIZE, pfn++) {
		rc = lkpi_vma_map(l, addr, (phys_addr_t)pfn << PAGE_SHIFT, NULL, prot);
		if (rc) {
			aurora_vma_dev_zap(l->avma, vma->vm_start, addr);
			return rc;
		}
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_remap_pfn_range);

int lkpi_vm_iomap_memory(struct vm_area_struct *vma, phys_addr_t start, unsigned long len)
{
	unsigned long vm_len = vma->vm_end - vma->vm_start;
	unsigned long pages = (len + (start & ~PAGE_MASK) + PAGE_SIZE - 1) >> PAGE_SHIFT;

	if (vma->vm_pgoff > pages || vm_len >> PAGE_SHIFT > pages - vma->vm_pgoff)
		return -EINVAL;
	return lkpi_remap_pfn_range(vma, vma->vm_start, (start >> PAGE_SHIFT) + vma->vm_pgoff,
				    vm_len, vma->vm_page_prot);
}
EXPORT_SYMBOL(lkpi_vm_iomap_memory);

int lkpi_vm_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page)
{
	if (addr < vma->vm_start || addr >= vma->vm_end || (addr & ~PAGE_MASK))
		return -EFAULT;
	vm_flags_set(vma, VM_MIXEDMAP);
	return lkpi_vma_map(to_lvma(vma), addr, page_to_phys(page), page, vma->vm_page_prot);
}
EXPORT_SYMBOL(lkpi_vm_insert_page);

vm_fault_t lkpi_vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
				    unsigned long pfn, pgprot_t prot)
{
	int rc;

	if (addr < vma->vm_start || addr >= vma->vm_end)
		return VM_FAULT_SIGBUS;
	rc = lkpi_vma_map(to_lvma(vma), addr & PAGE_MASK, (phys_addr_t)pfn << PAGE_SHIFT, NULL,
			  prot);
	if (rc == -ENOMEM)
		return VM_FAULT_OOM;
	return rc ? VM_FAULT_SIGBUS : VM_FAULT_NOPAGE;
}
EXPORT_SYMBOL(lkpi_vmf_insert_pfn_prot);

void lkpi_zap_vma_ptes(struct vm_area_struct *vma, unsigned long address, unsigned long size)
{
	struct lkpi_vma *l = to_lvma(vma);

	if (!size || address < vma->vm_start || address + size > vma->vm_end)
		return;
	aurora_vma_dev_zap(l->avma, address, address + size);
	lkpi_vma_put_range(l, address, address + size);
}
EXPORT_SYMBOL(lkpi_zap_vma_ptes);

void lkpi_unmap_mapping_range(struct address_space *mapping, loff_t holebegin, loff_t holelen,
			      int even_cows)
{
	pgoff_t first = holebegin >> PAGE_SHIFT, last;
	struct vm_area_struct *vma;

	last = holelen ? (holebegin + holelen - 1) >> PAGE_SHIFT : ULONG_MAX;
	mutex_lock(&mapping->i_mmap_mutex);
	list_for_each_entry(vma, &mapping->i_mmap, __lkpi_mapping) {
		struct lkpi_vma *l = to_lvma(vma);
		pgoff_t vfirst, vlast, a, b;

		lkpi_vma_sync(l);
		vfirst = vma->vm_pgoff;
		vlast = vfirst + vma_pages(vma) - 1;
		if (last < vfirst || first > vlast)
			continue;
		a = max(first, vfirst);
		b = min(last, vlast);
		aurora_vma_dev_zap(l->avma, vma->vm_start + ((a - vfirst) << PAGE_SHIFT),
				   vma->vm_start + ((b - vfirst + 1) << PAGE_SHIFT));
		lkpi_vma_put_range(l, vma->vm_start + ((a - vfirst) << PAGE_SHIFT),
				   vma->vm_start + ((b - vfirst + 1) << PAGE_SHIFT));
	}
	mutex_unlock(&mapping->i_mmap_mutex);
}
EXPORT_SYMBOL(lkpi_unmap_mapping_range);
