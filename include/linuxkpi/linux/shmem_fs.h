/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shmem: ficheros anonimos cuyas paginas salen del asignador de la capa
 * (mm/shmem.c). Sin swap: las paginas viven hasta que se libera el fichero
 * o se trunca el rango.
 */
#ifndef _LKPI_LINUX_SHMEM_FS_H
#define _LKPI_LINUX_SHMEM_FS_H

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mount.h>

struct file *lkpi_shmem_file_setup(const char *name, loff_t size, unsigned long flags);
void lkpi_shmem_truncate_range(struct inode *inode, loff_t start, loff_t end);

#define shmem_file_setup(n, s, f) lkpi_shmem_file_setup(n, s, f)
#define shmem_file_setup_with_mnt(m, n, s, f) lkpi_shmem_file_setup(n, s, f)
#define shmem_truncate_range(i, a, b) lkpi_shmem_truncate_range(i, a, b)

static inline struct folio *shmem_read_folio_gfp(struct address_space *m, pgoff_t index, gfp_t gfp)
{
	return lkpi_shmem_read_folio(m, index);
}
static inline struct folio *shmem_read_folio(struct address_space *m, pgoff_t index)
{
	return lkpi_shmem_read_folio(m, index);
}
static inline struct page *shmem_read_mapping_page_gfp(struct address_space *m, pgoff_t index, gfp_t gfp)
{
	return read_mapping_page(m, index, NULL);
}
#define shmem_read_mapping_page(m, i) shmem_read_mapping_page_gfp(m, i, 0)

#endif
