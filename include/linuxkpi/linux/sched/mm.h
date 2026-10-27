/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * current->mm y sus referencias (K8, L4). Cada proceso de Aurora tiene un
 * mm_struct de la capa, creado al pedirlo y validado por pid (ver
 * kernel/linuxkpi/kernel/sched.c). Los hilos del kernel no tienen mm.
 */
#ifndef _LKPI_LINUX_SCHED_MM_H
#define _LKPI_LINUX_SCHED_MM_H

#include <linux/sched.h>
#include <linux/mm_types.h>
#include <linux/gfp.h>

void lkpi_mmput(struct mm_struct *mm);
void lkpi_mmdrop(struct mm_struct *mm);
struct mm_struct *lkpi_get_task_mm(struct task_struct *p);

static inline void mmgrab(struct mm_struct *mm) { atomic_inc(&mm->mm_count); }
static inline void mmdrop(struct mm_struct *mm) { lkpi_mmdrop(mm); }
static inline void mmget(struct mm_struct *mm) { atomic_inc(&mm->mm_users); }
static inline bool mmget_not_zero(struct mm_struct *mm)
{
	return atomic_inc_not_zero(&mm->mm_users);
}
static inline void mmput(struct mm_struct *mm) { lkpi_mmput(mm); }
#define mmput_async(mm) mmput(mm)
static inline struct mm_struct *get_task_mm(struct task_struct *p)
{
	return lkpi_get_task_mm(p);
}

/* Sin reclamacion de memoria con E/S: los ambitos no cambian nada. */
static inline unsigned int memalloc_noio_save(void) { return 0; }
static inline void memalloc_noio_restore(unsigned int f) { (void)f; }
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int f) { (void)f; }
static inline unsigned int memalloc_noreclaim_save(void) { return 0; }
static inline void memalloc_noreclaim_restore(unsigned int f) { (void)f; }
#define might_alloc(gfp) do { (void)(gfp); } while (0)

#endif
