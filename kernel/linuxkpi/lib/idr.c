// SPDX-License-Identifier: GPL-2.0-only
/* IDR e IDA sobre xarray (ids desde idr_base; ptr NULL reserva el id). */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/idr.h>

void *lkpi_xa_load_raw(const struct xarray *xa, unsigned long index);

int lkpi_idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid, unsigned long max, gfp_t gfp)
{
	unsigned int base = idr->idr_base;
	u32 id;
	int ret;

	if (WARN_ON_ONCE(xa_is_internal(ptr)))
		return -EINVAL;
	if (max > UINT_MAX)
		max = UINT_MAX;
	if (max < base)
		return -ENOSPC;
	/* Como Linux: un inicio por debajo de la base empieza en la base. */
	id = *nextid < base ? 0 : *nextid - base;
	ret = xa_alloc(&idr->idr_rt, &id, ptr, XA_LIMIT(id, max - base), gfp);
	if (ret == -EBUSY)
		return -ENOSPC;
	if (ret)
		return ret;
	*nextid = id + base;
	return 0;
}
EXPORT_SYMBOL(lkpi_idr_alloc_u32);

int lkpi_idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
	u32 id = start;
	int ret;

	if (WARN_ON_ONCE(start < 0))
		return -EINVAL;
	ret = lkpi_idr_alloc_u32(idr, ptr, &id, end > 0 ? end - 1 : INT_MAX, gfp);
	return ret ? ret : (int)id;
}
EXPORT_SYMBOL(lkpi_idr_alloc);

int lkpi_idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
	u32 id = idr->idr_next;
	int err, max = end > 0 ? end - 1 : INT_MAX;

	if ((int)id < start)
		id = start;
	err = lkpi_idr_alloc_u32(idr, ptr, &id, max, gfp);
	if (err == -ENOSPC && id > (u32)start) {
		id = start;
		err = lkpi_idr_alloc_u32(idr, ptr, &id, max, gfp);
	}
	if (err)
		return err;
	idr->idr_next = id + 1;
	return id;
}
EXPORT_SYMBOL(lkpi_idr_alloc_cyclic);

void *lkpi_idr_find(const struct idr *idr, unsigned long id)
{
	if (id < idr->idr_base)
		return NULL;
	return xa_load((struct xarray *)&idr->idr_rt, id - idr->idr_base);
}
EXPORT_SYMBOL(lkpi_idr_find);

void *lkpi_idr_remove(struct idr *idr, unsigned long id)
{
	if (id < idr->idr_base)
		return NULL;
	return xa_erase(&idr->idr_rt, id - idr->idr_base);
}
EXPORT_SYMBOL(lkpi_idr_remove);

void *lkpi_idr_replace(struct idr *idr, void *ptr, unsigned long id)
{
	void *old;

	if (WARN_ON_ONCE(xa_is_internal(ptr)))
		return ERR_PTR(-EINVAL);
	if (id < idr->idr_base)
		return ERR_PTR(-ENOENT);
	id -= idr->idr_base;
	xa_lock(&idr->idr_rt);
	if (!lkpi_xa_load_raw(&idr->idr_rt, id)) {
		xa_unlock(&idr->idr_rt);
		return ERR_PTR(-ENOENT);
	}
	old = __xa_store(&idr->idr_rt, id, ptr, 0);
	xa_unlock(&idr->idr_rt);
	return old;
}
EXPORT_SYMBOL(lkpi_idr_replace);

void *lkpi_idr_get_next_ul(struct idr *idr, unsigned long *nextid)
{
	unsigned long id = *nextid < idr->idr_base ? 0 : *nextid - idr->idr_base;
	void *entry = xa_find(&idr->idr_rt, &id, ULONG_MAX, XA_PRESENT);

	if (entry)
		*nextid = id + idr->idr_base;
	return entry;
}
EXPORT_SYMBOL(lkpi_idr_get_next_ul);

void *lkpi_idr_get_next(struct idr *idr, int *nextid)
{
	unsigned long id = *nextid;
	void *entry = lkpi_idr_get_next_ul(idr, &id);

	if (entry && WARN_ON_ONCE(id > INT_MAX))
		return NULL;
	if (entry)
		*nextid = id;
	return entry;
}
EXPORT_SYMBOL(lkpi_idr_get_next);

int lkpi_idr_for_each(const struct idr *idr, int (*fn)(int id, void *p, void *data), void *data)
{
	unsigned long index;
	void *entry;

	xa_for_each((struct xarray *)&idr->idr_rt, index, entry) {
		int ret = fn(index + idr->idr_base, entry, data);
		if (ret)
			return ret;
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_idr_for_each);

void lkpi_idr_destroy(struct idr *idr)
{
	xa_destroy(&idr->idr_rt);
}
EXPORT_SYMBOL(lkpi_idr_destroy);

/* ---- IDA: cada id ocupado guarda un valor (no hay puntero) ---- */

int lkpi_ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max, gfp_t gfp)
{
	u32 id;
	int ret;

	if ((int)min < 0)
		return -ENOSPC;
	if ((int)max < 0)
		max = INT_MAX;
	ret = xa_alloc(&ida->xa, &id, xa_mk_value(0), XA_LIMIT(min, max), gfp);
	if (ret == -EBUSY)
		return -ENOSPC;
	return ret ? ret : (int)id;
}
EXPORT_SYMBOL(lkpi_ida_alloc_range);

void lkpi_ida_free(struct ida *ida, unsigned int id)
{
	WARN((int)id < 0 || !xa_erase(&ida->xa, id), "ida_free: id %u no asignado\n", id);
}
EXPORT_SYMBOL(lkpi_ida_free);

void lkpi_ida_destroy(struct ida *ida)
{
	xa_destroy(&ida->xa);
}
EXPORT_SYMBOL(lkpi_ida_destroy);
