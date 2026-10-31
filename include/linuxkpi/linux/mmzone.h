/* SPDX-License-Identifier: GPL-2.0-only */
/* Sin zonas ni nodos NUMA: MAX_PAGE_ORDER y NR_PAGE_ORDERS salen de gfp.h. */
#ifndef _LKPI_LINUX_MMZONE_H
#define _LKPI_LINUX_MMZONE_H

#include <linux/gfp.h>
#include <linux/mm_types.h>

#define MAX_NUMNODES 1
#define NUMA_NO_NODE (-1)

/* Un solo nodo y una sola zona con toda la RAM utilizable (N2, NVIDIA kernel-open). */
enum zone_type { ZONE_NORMAL, __MAX_NR_ZONES };
#define MAX_NR_ZONES __MAX_NR_ZONES
struct zone {
	unsigned long zone_start_pfn;
	unsigned long spanned_pages;
	unsigned long managed_pages;
	unsigned long present_pages;
};
enum { NR_FREE_PAGES };
#define ZONE_MOVABLE 1
static inline bool populated_zone(struct zone *z) { return z->present_pages != 0; }
static inline unsigned long zone_page_state_snapshot(struct zone *z, int item) { return z->managed_pages; }
static inline int page_zonenum(const struct page *p) { return ZONE_NORMAL; }
struct pglist_data {
	struct zone node_zones[MAX_NR_ZONES];
};
struct pglist_data *lkpi_node_data(int nid);
#define NODE_DATA(nid) lkpi_node_data(nid)
static inline bool managed_zone(struct zone *zone) { return zone->managed_pages != 0; }
static inline unsigned long zone_end_pfn(const struct zone *zone) { return zone->zone_start_pfn + zone->spanned_pages; }

#endif
