/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * HMM desactivado (L6): hmm_range_fault() siempre falla con -EOPNOTSUPP.
 * Los drivers que lo usan para userptr/SVM caen por la ruta de error.
 */
#ifndef _LKPI_LINUX_HMM_H
#define _LKPI_LINUX_HMM_H

#include <linux/mm_types.h>
#include <linux/mmu_notifier.h>
#include <linux/errno.h>

enum hmm_pfn_flags {
	HMM_PFN_VALID = 1UL << 63,
	HMM_PFN_WRITE = 1UL << 62,
	HMM_PFN_ERROR = 1UL << 61,
	HMM_PFN_ORDER_SHIFT = 56,
	HMM_PFN_REQ_FAULT = HMM_PFN_VALID,
	HMM_PFN_REQ_WRITE = HMM_PFN_WRITE,
	HMM_PFN_FLAGS = HMM_PFN_VALID | HMM_PFN_WRITE | HMM_PFN_ERROR,
};

struct hmm_range {
	struct mmu_interval_notifier *notifier;
	unsigned long notifier_seq;
	unsigned long start;
	unsigned long end;
	unsigned long *hmm_pfns;
	unsigned long default_flags;
	unsigned long pfn_flags_mask;
	void *dev_private_owner;
};

static inline int hmm_range_fault(struct hmm_range *range) { return -EOPNOTSUPP; }
static inline struct page *hmm_pfn_to_page(unsigned long hmm_pfn) { return NULL; }

#endif
