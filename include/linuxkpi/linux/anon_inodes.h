/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_ANON_INODES_H
#define _LKPI_LINUX_ANON_INODES_H

#include <linux/fs.h>

struct file *lkpi_anon_inode_getfile(const char *name, const struct file_operations *fops,
				     void *priv, int flags);
int lkpi_anon_inode_getfd(const char *name, const struct file_operations *fops, void *priv,
			  int flags);
#define anon_inode_getfile(n, f, p, fl) lkpi_anon_inode_getfile(n, f, p, fl)
#define anon_inode_getfd(n, f, p, fl) lkpi_anon_inode_getfd(n, f, p, fl)
#define anon_inode_create_getfile(n, f, p, fl, ctx) lkpi_anon_inode_getfile(n, f, p, fl)

#endif
