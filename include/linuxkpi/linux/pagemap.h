/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Cache de paginas minima (la de shmem_fs.h): address_space con un array de
 * paginas por indice. mapping_set_unevictable() y compania no hacen nada.
 */
#ifndef _LKPI_LINUX_PAGEMAP_H
#define _LKPI_LINUX_PAGEMAP_H

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/pagevec.h>
#include <linux/highmem.h>

#define AS_UNEVICTABLE 3

static inline gfp_t mapping_gfp_mask(struct address_space *m) { return m->gfp_mask; }
static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t mask) { m->gfp_mask = mask; }
static inline gfp_t mapping_gfp_constraint(struct address_space *m, gfp_t gfp_mask)
{
	return mapping_gfp_mask(m) & gfp_mask;
}
static inline void mapping_set_unevictable(struct address_space *m) { set_bit(AS_UNEVICTABLE, &m->flags); }
static inline void mapping_clear_unevictable(struct address_space *m)
{
	clear_bit(AS_UNEVICTABLE, &m->flags);
}
static inline bool mapping_unevictable(struct address_space *m)
{
	return m && test_bit(AS_UNEVICTABLE, &m->flags);
}

static inline struct page *folio_file_page(struct folio *folio, pgoff_t index)
{
	return folio_page(folio, index & (folio_nr_pages(folio) - 1));
}
static inline void folio_mark_dirty(struct folio *folio) { set_page_dirty(&folio->page); }
static inline void folio_mark_accessed(struct folio *folio) { }
static inline void check_move_unevictable_folios(struct folio_batch *fbatch) { }
static inline void mark_page_accessed(struct page *page) { }

static inline unsigned long invalidate_mapping_pages(struct address_space *m, pgoff_t start,
						     pgoff_t end)
{
	return 0;
}

struct folio *lkpi_shmem_read_folio(struct address_space *mapping, pgoff_t index);
static inline struct page *read_mapping_page(struct address_space *m, pgoff_t index, struct file *f)
{
	struct folio *folio = lkpi_shmem_read_folio(m, index);

	return IS_ERR(folio) ? ERR_CAST(folio) : &folio->page;
}

#endif
