/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_REF_TRACKER_H
#define _LKPI_LINUX_REF_TRACKER_H
/* CONFIG_REF_TRACKER=n: contadores sin traza; las API existen y no hacen nada. */
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/stackdepot.h>
struct ref_tracker;
struct ref_tracker_dir {
	spinlock_t lock;
	unsigned int quarantine_avail;
	refcount_t untracked;
	refcount_t no_tracker;
	bool dead;
	const char *class;
	char name[32];
};
static inline void ref_tracker_dir_init(struct ref_tracker_dir *dir, unsigned int quarantine_count,
					const char *name)
{
	spin_lock_init(&dir->lock);
	dir->quarantine_avail = quarantine_count;
	refcount_set(&dir->untracked, 1);
	refcount_set(&dir->no_tracker, 1);
	dir->dead = false;
	dir->class = name;
}
static inline void ref_tracker_dir_exit(struct ref_tracker_dir *dir) { dir->dead = true; }
static inline void ref_tracker_dir_print_locked(struct ref_tracker_dir *dir, unsigned int display_limit) {}
static inline void ref_tracker_dir_print(struct ref_tracker_dir *dir, unsigned int display_limit) {}
static inline int ref_tracker_dir_snprint(struct ref_tracker_dir *dir, char *buf, size_t size) { return 0; }
static inline int ref_tracker_alloc(struct ref_tracker_dir *dir, struct ref_tracker **trackerp, gfp_t gfp)
{
	refcount_inc(&dir->untracked);
	*trackerp = NULL;
	return 0;
}
static inline int ref_tracker_free(struct ref_tracker_dir *dir, struct ref_tracker **trackerp)
{
	refcount_dec(&dir->untracked);
	*trackerp = NULL;
	return 0;
}
static inline void ref_tracker_dir_symlink(struct ref_tracker_dir *dir, const char *fmt, ...) {}
#endif
