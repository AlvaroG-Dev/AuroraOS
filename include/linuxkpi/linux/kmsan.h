/* SPDX-License-Identifier: GPL-2.0-only */
/* KMSAN no existe en la capa: los avisos de DMA no hacen nada. */
#ifndef _LKPI_LINUX_KMSAN_H
#define _LKPI_LINUX_KMSAN_H

#include <linux/types.h>

static inline void kmsan_handle_dma(phys_addr_t phys, size_t size, int dir) { }

#endif
