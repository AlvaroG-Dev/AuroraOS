/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Banderas de pagina y de folio que usan shmem y TTM. Los folios de la capa
 * son paginas sueltas de un solo dueno (el fichero shmem o el pool de TTM): lock,
 * reclaim y writeback no tienen competencia y son no-ops; "dirty" ya existe
 * en set_page_dirty().
 */
#ifndef _LKPI_LINUX_PAGE_FLAGS_H
#define _LKPI_LINUX_PAGE_FLAGS_H

#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>

static inline void folio_lock(struct folio *folio) { }
static inline void folio_unlock(struct folio *folio) { }
static inline bool folio_trylock(struct folio *folio) { return true; }
static inline bool folio_mapped(const struct folio *folio) { return false; }
static inline bool folio_clear_dirty_for_io(struct folio *folio) { return true; }
static inline void folio_set_reclaim(struct folio *folio) { }
static inline void folio_clear_reclaim(struct folio *folio) { }
static inline bool folio_test_writeback(const struct folio *folio) { return false; }

#endif
