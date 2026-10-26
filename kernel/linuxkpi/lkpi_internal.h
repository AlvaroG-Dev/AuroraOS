/* SPDX-License-Identifier: GPL-2.0-only */
/* Declaraciones internas de linuxkpi.kur (no las ven los drivers). */
#ifndef _LKPI_INTERNAL_H
#define _LKPI_INTERNAL_H

#include <linux/types.h>

int lkpi_mm_init(void);
void lkpi_mm_exit(void);
int lkpi_slab_init(void);
void lkpi_slab_exit(void);
void lkpi_vmalloc_exit(void);
void lkpi_ioremap_exit(void);

/* K2 (kernel/): arranque y parada, en este orden (y el inverso al salir). */
int lkpi_sched_init(void);
void lkpi_sched_exit(void);
void lkpi_time_init(void);
void lkpi_wait_init(void);
int lkpi_softirq_init(void);
void lkpi_softirq_exit(void);
int lkpi_rcu_init(void);
void lkpi_rcu_exit(void);
int lkpi_wq_init(void);
void lkpi_wq_exit(void);

/* Hilos internos de la capa (kernel/sched.c). */
struct task_struct;
struct task_struct *lkpi_kthread_create_internal(int (*fn)(void *), void *data,
						 const char *name);

/* Registro pfn -> struct page (mm/page.c). */
#define LKPI_PAGE_OWNED 0x1	/* el array salio de lkpi_alloc_pages */
#define LKPI_PAGE_FOREIGN 0x2	/* descriptor suelto de un marco ajeno */
struct page;
struct page *lkpi_page_lookup(unsigned long pfn);
struct page *lkpi_page_alloc_desc(unsigned long pfn);
bool lkpi_is_direct_map(const void *addr);

#endif
