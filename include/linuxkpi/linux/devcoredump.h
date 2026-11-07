#ifndef _LKPI_DEVCOREDUMP_H
#define _LKPI_DEVCOREDUMP_H
#include <linux/device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
/* Sin devcoredump: se liberan los datos. */
static inline void dev_coredumpv(struct device *dev, void *data, size_t datalen, gfp_t gfp) { vfree(data); }
static inline void dev_coredumpsg(struct device *dev, struct scatterlist *table, size_t datalen, gfp_t gfp) { }
static inline void _devcd_free_sgtable(struct scatterlist *table) { }
#endif
