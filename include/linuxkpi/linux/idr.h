/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IDR (id -> puntero) e IDA (ids sin puntero), sobre xarray como en Linux.
 * Como alli, el IDR no se protege solo: el llamante serializa.
 */
#ifndef _LKPI_LINUX_IDR_H
#define _LKPI_LINUX_IDR_H

#include <linux/gfp_types.h>
#include <linux/xarray.h>

struct idr {
	struct xarray idr_rt;
	unsigned int idr_base;
	unsigned int idr_next;
};

#define IDR_INIT_BASE(name, base) { \
	.idr_rt = XARRAY_INIT(name.idr_rt, XA_FLAGS_ALLOC), \
	.idr_base = (base), .idr_next = 0 }
#define IDR_INIT(name) IDR_INIT_BASE(name, 0)
#define DEFINE_IDR(name) struct idr name = IDR_INIT(name)

static inline void idr_init_base(struct idr *idr, int base)
{
	xa_init_flags(&idr->idr_rt, XA_FLAGS_ALLOC);
	idr->idr_base = base;
	idr->idr_next = 0;
}
static inline void idr_init(struct idr *idr) { idr_init_base(idr, 0); }
static inline bool idr_is_empty(const struct idr *idr) { return xa_empty(&idr->idr_rt); }
static inline unsigned int idr_get_cursor(const struct idr *idr) { return READ_ONCE(idr->idr_next); }
static inline void idr_set_cursor(struct idr *idr, unsigned int val) { WRITE_ONCE(idr->idr_next, val); }
static inline void idr_preload(gfp_t gfp_mask) { }
static inline void idr_preload_end(void) { }

int lkpi_idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid, unsigned long max, gfp_t gfp);
int lkpi_idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
int lkpi_idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
void *lkpi_idr_find(const struct idr *idr, unsigned long id);
void *lkpi_idr_remove(struct idr *idr, unsigned long id);
void *lkpi_idr_replace(struct idr *idr, void *ptr, unsigned long id);
void *lkpi_idr_get_next(struct idr *idr, int *nextid);
void *lkpi_idr_get_next_ul(struct idr *idr, unsigned long *nextid);
int lkpi_idr_for_each(const struct idr *idr, int (*fn)(int id, void *p, void *data), void *data);
void lkpi_idr_destroy(struct idr *idr);

#define idr_alloc_u32 lkpi_idr_alloc_u32
#define idr_alloc lkpi_idr_alloc
#define idr_alloc_cyclic lkpi_idr_alloc_cyclic
#define idr_find lkpi_idr_find
#define idr_remove lkpi_idr_remove
#define idr_replace lkpi_idr_replace
#define idr_get_next lkpi_idr_get_next
#define idr_get_next_ul lkpi_idr_get_next_ul
#define idr_for_each lkpi_idr_for_each
#define idr_destroy lkpi_idr_destroy

#define idr_for_each_entry(idr, entry, id) \
	for (id = 0; ((entry) = idr_get_next(idr, &(id))) != NULL; id += 1U)
#define idr_for_each_entry_ul(idr, entry, tmp, id) \
	for (tmp = 0, id = 0; ((entry) = idr_get_next_ul(idr, &(id))) != NULL; id += 1)
#define idr_for_each_entry_continue(idr, entry, id) \
	for ((entry) = idr_get_next((idr), &(id)); entry; ++id, (entry) = idr_get_next((idr), &(id)))
#define idr_for_each_entry_continue_ul(idr, entry, tmp, id) \
	for (tmp = id; ((entry) = idr_get_next_ul(idr, &(id))) != NULL; id += 1)

/* --- IDA --- */
struct ida {
	struct xarray xa;
};
#define IDA_INIT_FLAGS (XA_FLAGS_LOCK_IRQ | XA_FLAGS_ALLOC)
#define IDA_INIT(name) { .xa = XARRAY_INIT(name, IDA_INIT_FLAGS) }
#define DEFINE_IDA(name) struct ida name = IDA_INIT(name)

int lkpi_ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max, gfp_t gfp);
void lkpi_ida_free(struct ida *ida, unsigned int id);
void lkpi_ida_destroy(struct ida *ida);

static inline void ida_init(struct ida *ida) { xa_init_flags(&ida->xa, IDA_INIT_FLAGS); }
static inline bool ida_is_empty(const struct ida *ida) { return xa_empty(&ida->xa); }
static inline int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max, gfp_t gfp)
{
	return lkpi_ida_alloc_range(ida, min, max, gfp);
}
static inline int ida_alloc(struct ida *ida, gfp_t gfp) { return ida_alloc_range(ida, 0, ~0U, gfp); }
static inline int ida_alloc_min(struct ida *ida, unsigned int min, gfp_t gfp)
{
	return ida_alloc_range(ida, min, ~0U, gfp);
}
static inline int ida_alloc_max(struct ida *ida, unsigned int max, gfp_t gfp)
{
	return ida_alloc_range(ida, 0, max, gfp);
}
static inline void ida_free(struct ida *ida, unsigned int id) { lkpi_ida_free(ida, id); }
static inline void ida_destroy(struct ida *ida) { lkpi_ida_destroy(ida); }
#define ida_simple_get(ida, start, end, gfp) \
	ida_alloc_range(ida, start, (end) ? (end) - 1 : INT_MAX, gfp)
#define ida_simple_remove(ida, id) ida_free(ida, id)

#endif
