/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shrinker minimo (L6). Una lista global de shrinkers registrados; no hay
 * LRU, cgroups ni nodos. lkpi_shrink_slab() recorre la lista cuando falla
 * una reserva de paginas de la capa (page.c) o cuando una prueba lo pide.
 */
#ifndef _LKPI_LINUX_SHRINKER_H
#define _LKPI_LINUX_SHRINKER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/atomic.h>

#define SHRINK_STOP (~0UL)
#define SHRINK_EMPTY (~0UL - 1)

struct shrink_control {
	gfp_t gfp_mask;
	int nid;
	unsigned long nr_to_scan;
	unsigned long nr_scanned;
	void *memcg;
};

struct shrinker {
	unsigned long (*count_objects)(struct shrinker *, struct shrink_control *sc);
	unsigned long (*scan_objects)(struct shrinker *, struct shrink_control *sc);
	long batch;
	int seeks;
	unsigned int flags;
	void *private_data;
	/* capa */
	struct list_head __lkpi_list;
	char __lkpi_name[32];
	bool __lkpi_registered;
	atomic_long_t __lkpi_scanned;	/* objetos liberados por scan_objects */
};

#define DEFAULT_SEEKS 2
#define SHRINKER_REGISTERED (1 << 0)
#define SHRINKER_NUMA_AWARE (1 << 2)
#define SHRINKER_MEMCG_AWARE (1 << 3)
#define SHRINKER_NONSLAB (1 << 4)

struct shrinker *lkpi_shrinker_alloc(unsigned int flags, const char *fmt, ...);
void lkpi_shrinker_register(struct shrinker *s);
void lkpi_shrinker_free(struct shrinker *s);
/* Pide hasta `nr` objetos a todos los shrinkers; devuelve los liberados. */
unsigned long lkpi_shrink_slab(gfp_t gfp, unsigned long nr);

#define shrinker_alloc(flags, fmt, ...) lkpi_shrinker_alloc(flags, fmt, ##__VA_ARGS__)
#define shrinker_register(s) lkpi_shrinker_register(s)
#define shrinker_free(s) lkpi_shrinker_free(s)

#endif
