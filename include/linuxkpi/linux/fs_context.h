/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_FS_CONTEXT_H
#define _LKPI_LINUX_FS_CONTEXT_H
#include <linux/fs.h>
#include <linux/mount.h>
/* Sin montajes propios de tmpfs: i915 sin hugepages (CONFIG_TRANSPARENT_HUGEPAGE=n) no los pide. */
#define SB_KERNMOUNT	(1 << 22)
static inline struct file_system_type *get_fs_type(const char *name) { return NULL; }
static inline struct fs_context *fs_context_for_mount(struct file_system_type *t, unsigned int sb_flags)
{
	return ERR_PTR(-ENODEV);
}
static inline int vfs_parse_fs_string(struct fs_context *fc, const char *key, const char *value) { return -ENODEV; }
static inline struct vfsmount *fc_mount_longterm(struct fs_context *fc) { return ERR_PTR(-ENODEV); }
static inline void put_fs_context(struct fs_context *fc) {}
static inline void kern_unmount(struct vfsmount *mnt) {}
#endif
