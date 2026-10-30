/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_PSEUDO_FS_H
#define _LKPI_LINUX_PSEUDO_FS_H

#include <linux/mount.h>

struct pseudo_fs_context {
	unsigned long magic;
};

/* drm_fs_init_fs_context() solo comprueba que no sea NULL */
static inline struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic)
{
	static struct pseudo_fs_context ctx;

	ctx.magic = magic;
	return &ctx;
}

#endif
