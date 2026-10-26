/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ASM_PAGE_H
#define _LKPI_ASM_PAGE_H

#include <linux/const.h>
#include <linux/types.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PMD_SHIFT 21
#define PMD_SIZE (_AC(1, UL) << PMD_SHIFT)
#define PMD_MASK (~(PMD_SIZE - 1))
#define PUD_SHIFT 30
#define PUD_SIZE (_AC(1, UL) << PUD_SHIFT)
#define PUD_MASK (~(PUD_SIZE - 1))
#define HPAGE_SHIFT PMD_SHIFT
#define HPAGE_SIZE PMD_SIZE
#define HPAGE_PMD_SIZE PMD_SIZE
#define HPAGE_PMD_ORDER (PMD_SHIFT - PAGE_SHIFT)
#define HPAGE_PMD_NR (1 << HPAGE_PMD_ORDER)

/* Ventana directa de RAM de Aurora (kernel/paging.h: PHYS_MAP_BASE). */
#define PAGE_OFFSET _AC(0xFFFF800000000000, UL)
#define __va(x) ((void *)((unsigned long)(x) + PAGE_OFFSET))
phys_addr_t lkpi_virt_to_phys(const void *addr);
#define __pa(x) lkpi_virt_to_phys((const void *)(x))
#define virt_to_phys(x) __pa(x)
#define phys_to_virt(x) __va(x)
#define PHYS_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))
#define PFN_PHYS(x) ((phys_addr_t)(x) << PAGE_SHIFT)
#define PFN_UP(x) (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x) ((x) >> PAGE_SHIFT)
#define PFN_ALIGN(x) (((unsigned long)(x) + (PAGE_SIZE - 1)) & PAGE_MASK)

#endif
