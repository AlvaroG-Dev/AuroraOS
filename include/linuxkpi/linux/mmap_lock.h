/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_MMAP_LOCK_H
#define _LKPI_LINUX_MMAP_LOCK_H

#include <linux/mm_types.h>
#include <linux/rwsem.h>

static inline void mmap_read_lock(struct mm_struct *mm) { down_read(&mm->mmap_lock); }
static inline int mmap_read_lock_killable(struct mm_struct *mm)
{
	return down_read_killable(&mm->mmap_lock);
}
static inline bool mmap_read_trylock(struct mm_struct *mm) { return down_read_trylock(&mm->mmap_lock); }
static inline void mmap_read_unlock(struct mm_struct *mm) { up_read(&mm->mmap_lock); }
static inline void mmap_write_lock(struct mm_struct *mm) { down_write(&mm->mmap_lock); }
static inline int mmap_write_lock_killable(struct mm_struct *mm)
{
	return down_write_killable(&mm->mmap_lock);
}
static inline void mmap_write_unlock(struct mm_struct *mm) { up_write(&mm->mmap_lock); }
static inline void mmap_assert_locked(struct mm_struct *mm) { (void)mm; }
static inline void mmap_assert_write_locked(struct mm_struct *mm) { (void)mm; }

#endif
