/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_MMU_CONTEXT_H
#define _LKPI_LINUX_MMU_CONTEXT_H
#include <linux/mm_types.h>
/* Los kthreads de la capa no adoptan el mm de otro proceso (userptr: L9). */
static inline void kthread_use_mm(struct mm_struct *mm) {}
static inline void kthread_unuse_mm(struct mm_struct *mm) {}
#endif
