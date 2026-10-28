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
		p = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!p) {
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

static const struct file_operations shmem_fops = {
	.release = shmem_release,
	.llseek = noop_llseek,
};

struct file *lkpi_shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
	struct file *f = lkpi_anon_inode_getfile(name, &shmem_fops, NULL, O_RDWR);

	if (IS_ERR(f))
		return f;
	f->f_mode |= FMODE_LSEEK;
	f->f_inode->i_size = size;
	return f;
}
EXPORT_SYMBOL(lkpi_shmem_file_setup);
