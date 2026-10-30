/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_WRITEBACK_H
#define _LKPI_LINUX_WRITEBACK_H
#include <linux/pagemap.h>
/* Sin disco de swap: no hay nada que escribir; writeback_iter() no produce folios. */
enum writeback_sync_modes { WB_SYNC_NONE, WB_SYNC_ALL };
struct writeback_control {
	long nr_to_write;
	long pages_skipped;
	loff_t range_start;
	loff_t range_end;
	enum writeback_sync_modes sync_mode;
	unsigned for_kupdate:1, range_cyclic:1, for_reclaim:1;
};
static inline struct folio *writeback_iter(struct address_space *mapping, struct writeback_control *wbc,
					   struct folio *folio, int *error)
{
	return NULL;
}
static inline void folio_redirty_for_writepage(struct writeback_control *wbc, struct folio *folio) {}
#endif
