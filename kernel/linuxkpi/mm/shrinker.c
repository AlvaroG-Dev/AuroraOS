// SPDX-License-Identifier: GPL-2.0-only
/*
 * shrinker minimo (L6): lista global de shrinkers, sin LRU, cgroups ni nodos.
 * lkpi_shrink_slab() pregunta a cada uno cuantos objetos puede soltar
 * (count_objects) y le pide los que falten hasta `nr` (scan_objects). Lo llama
 * lkpi_alloc_pages() cuando el PMM no tiene memoria y las pruebas a mano.
 */
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/stdarg.h>

static DEFINE_MUTEX(shrinker_lock);
static LIST_HEAD(shrinker_list);

struct shrinker *lkpi_shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	struct shrinker *s = kzalloc(sizeof(*s), GFP_KERNEL);
	va_list ap;

	if (!s)
		return NULL;
	s->flags = flags;
	s->batch = 128;
	s->seeks = DEFAULT_SEEKS;
	INIT_LIST_HEAD(&s->__lkpi_list);
	va_start(ap, fmt);
	vsnprintf(s->__lkpi_name, sizeof(s->__lkpi_name), fmt, ap);
	va_end(ap);
	return s;
}
EXPORT_SYMBOL(lkpi_shrinker_alloc);

void lkpi_shrinker_register(struct shrinker *s)
{
	mutex_lock(&shrinker_lock);
	list_add_tail(&s->__lkpi_list, &shrinker_list);
	s->__lkpi_registered = true;
	s->flags |= SHRINKER_REGISTERED;
	mutex_unlock(&shrinker_lock);
}
EXPORT_SYMBOL(lkpi_shrinker_register);

void lkpi_shrinker_free(struct shrinker *s)
{
	if (!s)
		return;
	mutex_lock(&shrinker_lock);
	if (s->__lkpi_registered)
		list_del(&s->__lkpi_list);
	mutex_unlock(&shrinker_lock);
	kfree(s);
}
EXPORT_SYMBOL(lkpi_shrinker_free);

unsigned long lkpi_shrink_slab(gfp_t gfp, unsigned long nr)
{
	static atomic_t running;
	struct shrinker *s;
	unsigned long freed = 0;

	/* Un shrinker puede reservar memoria (swap a shmem) y volver aqui. */
	if (atomic_cmpxchg(&running, 0, 1))
		return 0;
	mutex_lock(&shrinker_lock);
	list_for_each_entry(s, &shrinker_list, __lkpi_list) {
		struct shrink_control sc = { .gfp_mask = gfp, .nr_to_scan = nr - freed };
		unsigned long cnt, got;

		if (!s->count_objects || !s->scan_objects)
			continue;
		cnt = s->count_objects(s, &sc);
		if (!cnt || cnt == SHRINK_EMPTY)
			continue;
		sc.nr_to_scan = min(cnt, nr - freed);
		got = s->scan_objects(s, &sc);
		if (got == SHRINK_STOP)
			continue;
		atomic_long_add(got, &s->__lkpi_scanned);
		freed += got;
		if (freed >= nr)
			break;
	}
	mutex_unlock(&shrinker_lock);
	atomic_set(&running, 0);
	return freed;
}
EXPORT_SYMBOL(lkpi_shrink_slab);
