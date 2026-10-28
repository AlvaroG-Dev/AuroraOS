/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Montajes minimos: solo lo que los pseudo-fs de los drivers (drm_drv) usan
 * para obtener un super_block del que colgar inodos anonimos. No hay VFS de
 * Linux detras: simple_pin_fs() fabrica un vfsmount/super_block de la capa.
 */
#ifndef _LKPI_LINUX_MOUNT_H
#define _LKPI_LINUX_MOUNT_H

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/module.h>

struct fs_context {
	void *s_fs_info;
	unsigned int sb_flags;
};

struct super_block {
	unsigned long s_magic;
	void *s_fs_info;
	const struct file_system_type *s_type;
};

struct file_system_type {
	const char *name;
	int fs_flags;
	int (*init_fs_context)(struct fs_context *fc);
	void (*kill_sb)(struct super_block *sb);
	struct module *owner;
};

struct vfsmount {
	struct super_block *mnt_sb;
};

static inline void kill_anon_super(struct super_block *sb) { }

static inline int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount, int *count)
{
	if (!*mount) {
		struct vfsmount *m = kzalloc(sizeof(*m) + sizeof(struct super_block), GFP_KERNEL);

		if (!m)
			return -ENOMEM;
		m->mnt_sb = (struct super_block *)(m + 1);
		m->mnt_sb->s_type = type;
		*mount = m;
	}
	(*count)++;
	return 0;
}

static inline void simple_release_fs(struct vfsmount **mount, int *count)
{
	if (--*count == 0) {
		kfree(*mount);
		*mount = NULL;
	}
}

#endif
