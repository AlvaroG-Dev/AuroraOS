/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mmu_notifier (L6): la capa no avisa de invalidaciones de la tabla de
 * paginas de un proceso. Los notificadores se pueden registrar y los
 * intervalos insertar (los drivers de GPU lo hacen para userptr/SVM), pero
 * nunca reciben llamadas: mmu_interval_read_begin() devuelve siempre la misma
 * secuencia y mmu_interval_read_retry() dice que no hace falta reintentar.
 * Con HMM desactivado (linux/hmm.h) el camino de userptr no se usa.
 */
#ifndef _LKPI_LINUX_MMU_NOTIFIER_H
#define _LKPI_LINUX_MMU_NOTIFIER_H

#include <linux/mm_types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/errno.h>

struct mmu_notifier;
struct mmu_interval_notifier;

enum mmu_notifier_event {
	MMU_NOTIFY_UNMAP = 0,
	MMU_NOTIFY_CLEAR,
	MMU_NOTIFY_PROTECTION_VMA,
	MMU_NOTIFY_PROTECTION_PAGE,
	MMU_NOTIFY_SOFT_DIRTY,
	MMU_NOTIFY_RELEASE,
	MMU_NOTIFY_MIGRATE,
	MMU_NOTIFY_EXCLUSIVE,
};

#define MMU_NOTIFIER_RANGE_BLOCKABLE (1 << 0)

struct mmu_notifier_range {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	unsigned int flags;
	enum mmu_notifier_event event;
	void *owner;
};

struct mmu_notifier_ops {
	void (*release)(struct mmu_notifier *subscription, struct mm_struct *mm);
	int (*invalidate_range_start)(struct mmu_notifier *subscription,
				      const struct mmu_notifier_range *range);
	void (*invalidate_range_end)(struct mmu_notifier *subscription,
				     const struct mmu_notifier_range *range);
	void (*arch_invalidate_secondary_tlbs)(struct mmu_notifier *subscription,
					       struct mm_struct *mm, unsigned long start,
					       unsigned long end);
	struct mmu_notifier *(*alloc_notifier)(struct mm_struct *mm);
	void (*free_notifier)(struct mmu_notifier *subscription);
};

struct mmu_notifier {
	struct hlist_node hlist;
	const struct mmu_notifier_ops *ops;
	struct mm_struct *mm;
	unsigned int users;
};

struct mmu_interval_notifier_ops {
	bool (*invalidate)(struct mmu_interval_notifier *interval_sub,
			   const struct mmu_notifier_range *range, unsigned long cur_seq);
};

struct mmu_interval_notifier {
	const struct mmu_interval_notifier_ops *ops;
	struct mm_struct *mm;
	unsigned long start;
	unsigned long last;
	unsigned long invalidate_seq;
};

static inline int mmu_notifier_register(struct mmu_notifier *sub, struct mm_struct *mm)
{
	sub->mm = mm;
	return 0;
}
static inline int __mmu_notifier_register(struct mmu_notifier *sub, struct mm_struct *mm)
{
	return mmu_notifier_register(sub, mm);
}
static inline void mmu_notifier_unregister(struct mmu_notifier *sub, struct mm_struct *mm) { }
static inline void mmu_notifier_put(struct mmu_notifier *sub) { }
static inline void mmu_notifier_synchronize(void) { }

static inline int mmu_interval_notifier_insert(struct mmu_interval_notifier *sub,
					       struct mm_struct *mm, unsigned long start,
					       unsigned long length,
					       const struct mmu_interval_notifier_ops *ops)
{
	sub->mm = mm;
	sub->ops = ops;
	sub->start = start;
	sub->last = start + length - 1;
	sub->invalidate_seq = 1;
	return 0;
}
static inline int mmu_interval_notifier_insert_locked(struct mmu_interval_notifier *sub,
						      struct mm_struct *mm, unsigned long start,
						      unsigned long length,
						      const struct mmu_interval_notifier_ops *ops)
{
	return mmu_interval_notifier_insert(sub, mm, start, length, ops);
}
static inline void mmu_interval_notifier_remove(struct mmu_interval_notifier *sub) { }
static inline unsigned long mmu_interval_read_begin(struct mmu_interval_notifier *sub)
{
	return sub->invalidate_seq;
}
static inline bool mmu_interval_read_retry(struct mmu_interval_notifier *sub, unsigned long seq)
{
	return sub->invalidate_seq != seq;
}
static inline bool mmu_interval_check_retry(struct mmu_interval_notifier *sub, unsigned long seq)
{
	return READ_ONCE(sub->invalidate_seq) != seq;
}
static inline void mmu_interval_set_seq(struct mmu_interval_notifier *sub, unsigned long cur_seq)
{
	WRITE_ONCE(sub->invalidate_seq, cur_seq);
}

static inline void mmu_notifier_range_init(struct mmu_notifier_range *range,
					   enum mmu_notifier_event event, unsigned int flags,
					   struct mm_struct *mm, unsigned long start,
					   unsigned long end)
{
	range->mm = mm;
	range->start = start;
	range->end = end;
	range->flags = flags;
	range->event = event;
	range->owner = NULL;
}
static inline bool mmu_notifier_range_blockable(const struct mmu_notifier_range *range)
{
	return range->flags & MMU_NOTIFIER_RANGE_BLOCKABLE;
}
static inline void mmu_notifier_invalidate_range_start(struct mmu_notifier_range *range) { }
static inline void mmu_notifier_invalidate_range_end(struct mmu_notifier_range *range) { }
static inline void mmu_notifier_arch_invalidate_secondary_tlbs(struct mm_struct *mm,
							       unsigned long start, unsigned long end)
{
}

#endif
