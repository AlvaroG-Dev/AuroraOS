/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SORT_H
#define _LKPI_LINUX_SORT_H

#include <linux/types.h>

typedef void (*swap_func_t)(void *a, void *b, int size);
typedef int (*cmp_func_t)(const void *a, const void *b);
typedef int (*cmp_r_func_t)(const void *a, const void *b, const void *priv);

void lkpi_sort_r(void *base, size_t num, size_t size, cmp_r_func_t cmp,
		 swap_func_t swap, const void *priv);
void lkpi_sort(void *base, size_t num, size_t size, cmp_func_t cmp, swap_func_t swap);
#define sort lkpi_sort
#define sort_r lkpi_sort_r
#define sort_nonatomic lkpi_sort

struct list_head;
void lkpi_list_sort(void *priv, struct list_head *head,
		    int (*cmp)(void *priv, const struct list_head *a, const struct list_head *b));
#define list_sort lkpi_list_sort

#endif
