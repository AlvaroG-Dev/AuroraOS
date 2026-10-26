// SPDX-License-Identifier: GPL-2.0-only
/* sort() (heapsort, no estable, sin memoria extra) y list_sort() (mergesort estable). */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/sort.h>

static void generic_swap(void *a, void *b, int size)
{
	char *x = a, *y = b;
	while (size--) {
		char t = *x;
		*x++ = *y;
		*y++ = t;
	}
}

void lkpi_sort_r(void *base, size_t num, size_t size, cmp_r_func_t cmp,
		 swap_func_t swap_fn, const void *priv)
{
	char *b = base;
	size_t n = num, i, parent, child;

	if (num < 2)
		return;
	if (!swap_fn)
		swap_fn = generic_swap;
	/* Montculo maximo con la raiz en 0. */
	for (i = n / 2; i-- > 0;) {
		for (parent = i; (child = 2 * parent + 1) < n; parent = child) {
			if (child + 1 < n && cmp(b + child * size, b + (child + 1) * size, priv) < 0)
				child++;
			if (cmp(b + parent * size, b + child * size, priv) >= 0)
				break;
			swap_fn(b + parent * size, b + child * size, (int)size);
		}
	}
	while (--n > 0) {
		swap_fn(b, b + n * size, (int)size);
		for (parent = 0; (child = 2 * parent + 1) < n; parent = child) {
			if (child + 1 < n && cmp(b + child * size, b + (child + 1) * size, priv) < 0)
				child++;
			if (cmp(b + parent * size, b + child * size, priv) >= 0)
				break;
			swap_fn(b + parent * size, b + child * size, (int)size);
		}
	}
}
EXPORT_SYMBOL(lkpi_sort_r);

static int cmp_wrapper(const void *a, const void *b, const void *priv)
{
	return ((cmp_func_t)priv)(a, b);
}

void lkpi_sort(void *base, size_t num, size_t size, cmp_func_t cmp, swap_func_t swap_fn)
{
	lkpi_sort_r(base, num, size, cmp_wrapper, swap_fn, (const void *)cmp);
}
EXPORT_SYMBOL(lkpi_sort);

typedef int (*list_cmp_func_t)(void *, const struct list_head *, const struct list_head *);

/* Mezcla dos listas simples (terminadas en NULL, por ->next) de forma estable. */
static struct list_head *merge(void *priv, list_cmp_func_t cmp,
			       struct list_head *a, struct list_head *b)
{
	struct list_head *head = NULL, **tail = &head;
	while (a && b) {
		if (cmp(priv, a, b) <= 0) {
			*tail = a;
			tail = &a->next;
			a = a->next;
		} else {
			*tail = b;
			tail = &b->next;
			b = b->next;
		}
	}
	*tail = a ? a : b;
	return head;
}

void lkpi_list_sort(void *priv, struct list_head *head, list_cmp_func_t cmp)
{
	struct list_head *bins[64] = { NULL };
	struct list_head *list, *next, *prev;
	int i, max_bin = 0;

	if (list_empty(head) || list_is_singular(head))
		return;
	head->prev->next = NULL;
	list = head->next;
	/* Mergesort de abajo arriba: bins[i] tiene 2^i elementos. */
	while (list) {
		struct list_head *cur = list;
		list = list->next;
		cur->next = NULL;
		for (i = 0; i < 64 && bins[i]; i++) {
			cur = merge(priv, cmp, bins[i], cur);
			bins[i] = NULL;
		}
		if (i == 64)
			i = 63;
		bins[i] = cur;
		if (i > max_bin)
			max_bin = i;
	}
	list = NULL;
	for (i = 0; i <= max_bin; i++)
		if (bins[i])
			list = list ? merge(priv, cmp, bins[i], list) : bins[i];
	/* Rehace los enlaces prev y cierra el circulo. */
	prev = head;
	for (; list; list = next) {
		next = list->next;
		list->prev = prev;
		prev->next = list;
		prev = list;
	}
	prev->next = head;
	head->prev = prev;
}
EXPORT_SYMBOL(lkpi_list_sort);
