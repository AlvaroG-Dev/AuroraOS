// SPDX-License-Identifier: GPL-2.0-only
/*
 * shmem (K9-drm, L5): ficheros anonimos con paginas de lkpi_alloc_pages().
 * El address_space guarda un array de paginas por indice; las paginas se
 * crean (a cero) al leerlas y se liberan al truncar o al cerrar el fichero.
 */
#include <linux/anon_inodes.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/kstrtox.h>
#include <linux/printk.h>
#include "../aurora.h"

/* ------------------------------------------ limite por cliente (L9.4)
 *
 * Cada fichero shmem (los objetos GEM de i915 y drm_gem_shmem) se cobra al
 * proceso de usuario que lo creo. Un proceso no puede tener mas de
 * `lkpi.client_mem=<MiB>` en paginas shmem de la capa (por omision la mitad de
 * la RAM; 0 = sin limite): un cliente de GPU descontrolado recibe -ENOMEM en
 * vez de dejar el sistema sin memoria. Lo creado desde hilos del kernel no
 * se cobra.
 */
#define SHMEM_ACCT_SLOTS 128
static struct {
	pid_t owner;
	unsigned long pages;
} shmem_acct[SHMEM_ACCT_SLOTS];
static DEFINE_SPINLOCK(shmem_acct_lock);
static unsigned long shmem_limit_pages = ~0UL;	/* ~0 = sin leer */
struct lkpi_shmem_stats lkpi_shmem_stats;
EXPORT_SYMBOL(lkpi_shmem_stats);

static unsigned long shmem_client_limit(void)
{
	char v[24];
	unsigned long mb;

	if (shmem_limit_pages != ~0UL)
		return shmem_limit_pages;
	if (aurora_cmdline_get("lkpi.client_mem", v, sizeof(v)) && !kstrtoul(v, 10, &mb))
		shmem_limit_pages = mb << (20 - PAGE_SHIFT);	/* 0 = sin limite */
	else
		shmem_limit_pages = aurora_pmm_total_pages() / 2;
	return shmem_limit_pages;
}

/* Para pruebas: fija el limite en paginas (0 = sin limite); devuelve el anterior. */
unsigned long lkpi_shmem_set_client_limit(unsigned long pages)
{
	unsigned long old = shmem_client_limit();

	shmem_limit_pages = pages;
	return old;
}
EXPORT_SYMBOL(lkpi_shmem_set_client_limit);

unsigned long lkpi_shmem_client_pages(pid_t owner)
{
	unsigned long flags, n = 0;
	int i;

	spin_lock_irqsave(&shmem_acct_lock, flags);
	for (i = 0; i < SHMEM_ACCT_SLOTS; i++)
		if (shmem_acct[i].owner == owner && shmem_acct[i].pages)
			n = shmem_acct[i].pages;
	spin_unlock_irqrestore(&shmem_acct_lock, flags);
	return n;
}
EXPORT_SYMBOL(lkpi_shmem_client_pages);

/* Para pruebas; solo antes de que el fichero tenga paginas. */
void lkpi_shmem_set_owner(struct file *f, pid_t owner)
{
	f->f_mapping->__lkpi_owner = owner;
}
EXPORT_SYMBOL(lkpi_shmem_set_owner);

/* Cobra una pagina a `owner`; false si supera su limite. */
static bool shmem_charge(pid_t owner)
{
	unsigned long flags, limit = shmem_client_limit();
	int i, slot = -1;
	bool ok = true;

	if (!owner)
		return true;
	spin_lock_irqsave(&shmem_acct_lock, flags);
	for (i = 0; i < SHMEM_ACCT_SLOTS; i++) {
		if (shmem_acct[i].pages && shmem_acct[i].owner == owner) {
			slot = i;
			break;
		}
		if (!shmem_acct[i].pages && slot < 0)
			slot = i;
	}
	if (slot >= 0) {	/* tabla llena: ese cliente queda sin cobrar */
		if (shmem_acct[slot].owner != owner || !shmem_acct[slot].pages) {
			shmem_acct[slot].owner = owner;
			shmem_acct[slot].pages = 0;
		}
		if (limit && shmem_acct[slot].pages >= limit)
			ok = false;
		else
			shmem_acct[slot].pages++;
	}
	spin_unlock_irqrestore(&shmem_acct_lock, flags);
	if (!ok) {
		lkpi_shmem_stats.denied++;
		pr_warn_ratelimited("linuxkpi: el proceso %d supera su limite de memoria shmem (%lu MiB)\n",
				    owner, limit >> (20 - PAGE_SHIFT));
	}
	return ok;
}

static void shmem_uncharge(pid_t owner)
{
	unsigned long flags;
	int i;

	if (!owner)
		return;
	spin_lock_irqsave(&shmem_acct_lock, flags);
	for (i = 0; i < SHMEM_ACCT_SLOTS; i++)
		if (shmem_acct[i].pages && shmem_acct[i].owner == owner) {
			shmem_acct[i].pages--;
			break;
		}
	spin_unlock_irqrestore(&shmem_acct_lock, flags);
}

static void shmem_drop_range(struct address_space *m, pgoff_t first, pgoff_t last)
{
	pgoff_t i;

	mutex_lock(&m->__lkpi_pages_lock);
	for (i = first; i < m->__lkpi_cap && i <= last; i++) {
		struct page *p = m->__lkpi_pages[i];

		if (!p)
			continue;
		m->__lkpi_pages[i] = NULL;
		m->nrpages--;
		shmem_uncharge(m->__lkpi_owner);
		put_page(p);
	}
	mutex_unlock(&m->__lkpi_pages_lock);
}

struct folio *lkpi_shmem_read_folio(struct address_space *m, pgoff_t index)
{
	struct page *p;

	mutex_lock(&m->__lkpi_pages_lock);
	if (index >= m->__lkpi_cap) {
		unsigned long cap = max_t(unsigned long, 64, roundup_pow_of_two(index + 1));
		struct page **np = kvzalloc(cap * sizeof(*np), GFP_KERNEL);

		if (!np) {
			mutex_unlock(&m->__lkpi_pages_lock);
			return ERR_PTR(-ENOMEM);
		}
		if (m->__lkpi_pages)
			memcpy(np, m->__lkpi_pages, m->__lkpi_cap * sizeof(*np));
		kvfree(m->__lkpi_pages);
		m->__lkpi_pages = np;
		m->__lkpi_cap = cap;
	}
	p = m->__lkpi_pages[index];
	if (!p) {
		bool charged = m->__lkpi_owner && shmem_charge(m->__lkpi_owner);

		if (m->__lkpi_owner && !charged) {
			mutex_unlock(&m->__lkpi_pages_lock);
			return ERR_PTR(-ENOMEM);
		}
		p = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!p) {
			if (charged)
				shmem_uncharge(m->__lkpi_owner);
			mutex_unlock(&m->__lkpi_pages_lock);
			return ERR_PTR(-ENOMEM);
		}
		p->mapping = m;
		p->index = index;
		m->__lkpi_pages[index] = p;
		m->nrpages++;
	}
	get_page(p);	/* la del llamante; la de la cache sigue */
	mutex_unlock(&m->__lkpi_pages_lock);
	return page_folio(p);
}
EXPORT_SYMBOL(lkpi_shmem_read_folio);

void lkpi_shmem_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
	pgoff_t first = (start + PAGE_SIZE - 1) >> PAGE_SHIFT;
	pgoff_t last = end == (loff_t)-1 ? ~0UL : ((end + 1) >> PAGE_SHIFT) - 1;

	if (end != (loff_t)-1 && (end + 1) >> PAGE_SHIFT == 0)
		return;
	unmap_mapping_range(inode->i_mapping, start, end == (loff_t)-1 ? 0 : end - start + 1, 1);
	shmem_drop_range(inode->i_mapping, first, last);
}
EXPORT_SYMBOL(lkpi_shmem_truncate_range);

static int shmem_release(struct inode *inode, struct file *file)
{
	struct address_space *m = file->f_mapping;

	shmem_drop_range(m, 0, ~0UL);
	kvfree(m->__lkpi_pages);
	m->__lkpi_pages = NULL;
	m->__lkpi_cap = 0;
	return 0;
}

/* mmap de usuario de un fichero shmem (vm_mmap de GEM_MMAP legacy de i915). */
static vm_fault_t shmem_fault(struct vm_fault *vmf)
{
	struct file *file = vmf->vma->vm_file;
	struct folio *folio;

	if (vmf->pgoff >= DIV_ROUND_UP(i_size_read(file->f_inode), PAGE_SIZE))
		return VM_FAULT_SIGBUS;
	folio = lkpi_shmem_read_folio(file->f_mapping, vmf->pgoff);
	if (IS_ERR(folio))
		return VM_FAULT_OOM;
	vmf->page = &folio->page;	/* con la referencia de la lectura */
	return 0;
}

static const struct vm_operations_struct shmem_vm_ops = {
	.fault = shmem_fault,
};

static int shmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	vma->vm_ops = &shmem_vm_ops;
	return 0;
}

static const struct file_operations shmem_fops = {
	.release = shmem_release,
	.mmap = shmem_mmap,
	.llseek = noop_llseek,
};

struct file *lkpi_shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
	struct file *f = lkpi_anon_inode_getfile(name, &shmem_fops, NULL, O_RDWR);

	if (IS_ERR(f))
		return f;
	f->f_mode |= FMODE_LSEEK;
	f->f_inode->i_size = size;
	/* Se cobra al proceso de usuario que lo crea (el cliente de DRM). */
	f->f_mapping->__lkpi_owner = current->mm ? task_tgid_nr(current) : 0;
	return f;
}
EXPORT_SYMBOL(lkpi_shmem_file_setup);

/* Escritura de kernel en un fichero shmem de la capa (cache de paginas propia). */
ssize_t lkpi_kernel_write(struct file *file, const void *buf, size_t count, loff_t *pos)
{
	struct address_space *m = file->f_mapping;
	const char *src = buf;
	size_t done = 0;

	while (done < count) {
		pgoff_t idx = (*pos + done) >> PAGE_SHIFT;
		size_t off = (*pos + done) & ~PAGE_MASK, n = min_t(size_t, PAGE_SIZE - off, count - done);
		struct folio *folio = lkpi_shmem_read_folio(m, idx);

		if (IS_ERR(folio))
			return done ? (ssize_t)done : PTR_ERR(folio);
		memcpy((char *)page_address(&folio->page) + off, src + done, n);
		put_page(&folio->page);
		done += n;
	}
	*pos += done;
	if (*pos > i_size_read(file->f_inode))
		file->f_inode->i_size = *pos;
	return done;
}
EXPORT_SYMBOL(lkpi_kernel_write);
