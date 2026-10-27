/* SPDX-License-Identifier: GPL-2.0-only */
/* Interno de la capa: ficheros (fs/file.c), /dev (char_dev.c), debugfs. */
#ifndef _LKPI_FS_INTERNAL_H
#define _LKPI_FS_INTERNAL_H

#include <linux/fs.h>
#include "../aurora.h"

/*
 * Origen de un nodo que todavia no es un fichero abierto (/dev/<x>, una
 * entrada de debugfs): Aurora copia el puntero a node->priv en cada lookup;
 * ->open crea el struct file y lo sustituye. Va siempre como primer campo.
 */
#define LKPI_VNODE_MAGIC 0x4c4b5643u	/* "LKVC" */
struct lkpi_vnode_src {
	u32 magic;
	/* Crea y abre el fichero (llama a fops->open); ERR_PTR si falla. */
	struct file *(*open)(struct lkpi_vnode_src *src, unsigned int flags);
};

extern const struct aurora_vfs_ops lkpi_file_vops;

/* struct file nuevo con su inode propio; aun sin nodo (lo pone el llamante). */
struct file *lkpi_file_alloc(const struct file_operations *fops, unsigned int flags);
void lkpi_file_free(struct file *f);
/* Nodo anonimo de Aurora para `f` (nombre, modo); toma la referencia inicial. */
int lkpi_file_attach_node(struct file *f, const char *name, u32 vflags, u32 mode, u64 size);
/* struct file de un nodo de la capa, o NULL. */
struct file *lkpi_file_of_node(struct aurora_vfs_node *n);
int lkpi_errno_user(long rc);

/* mm/mmap.c */
int lkpi_file_mmap(struct file *file, struct aurora_process *proc, struct aurora_vma *avma,
		   u64 prot, u64 flags);

/* char_dev.c */
int lkpi_chrdev_init(void);
void lkpi_chrdev_exit(void);
struct device;
int lkpi_devnode_add(struct device *dev);
void lkpi_devnode_del(struct device *dev);
int lkpi_debugfs_init(void);
void lkpi_debugfs_exit(void);

#endif
