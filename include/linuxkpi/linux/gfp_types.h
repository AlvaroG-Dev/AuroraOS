/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Flags de asignacion. La capa solo distingue __GFP_ZERO, __GFP_NOWARN y
 * __GFP_DMA32/__GFP_DMA (memoria por debajo de 4 GiB / 16 MiB: L4). Las de
 * reclamacion/recuperacion se aceptan y se ignoran (Aurora no reclama).
 */
#ifndef _LKPI_LINUX_GFP_TYPES_H
#define _LKPI_LINUX_GFP_TYPES_H

#include <linux/types.h>

#define ___GFP_DMA 0x01u
#define ___GFP_HIGHMEM 0x02u
#define ___GFP_DMA32 0x04u
#define ___GFP_MOVABLE 0x08u
#define ___GFP_RECLAIMABLE 0x10u
#define ___GFP_HIGH 0x20u
#define ___GFP_IO 0x40u
#define ___GFP_FS 0x80u
#define ___GFP_ZERO 0x100u
#define ___GFP_DIRECT_RECLAIM 0x400u
#define ___GFP_KSWAPD_RECLAIM 0x800u
#define ___GFP_WRITE 0x1000u
#define ___GFP_NOWARN 0x2000u
#define ___GFP_RETRY_MAYFAIL 0x4000u
#define ___GFP_NOFAIL 0x8000u
#define ___GFP_NORETRY 0x10000u
#define ___GFP_MEMALLOC 0x20000u
#define ___GFP_COMP 0x40000u
#define ___GFP_NOMEMALLOC 0x80000u
#define ___GFP_HARDWALL 0x100000u
#define ___GFP_THISNODE 0x200000u
#define ___GFP_ACCOUNT 0x400000u
#define ___GFP_ZEROTAGS 0x800000u
#define ___GFP_NOLOCKDEP 0x1000000u

#define __GFP_DMA ((gfp_t)___GFP_DMA)
#define __GFP_HIGHMEM ((gfp_t)___GFP_HIGHMEM)
#define __GFP_DMA32 ((gfp_t)___GFP_DMA32)
#define __GFP_MOVABLE ((gfp_t)___GFP_MOVABLE)
#define __GFP_RECLAIMABLE ((gfp_t)___GFP_RECLAIMABLE)
#define __GFP_HIGH ((gfp_t)___GFP_HIGH)
#define __GFP_IO ((gfp_t)___GFP_IO)
#define __GFP_FS ((gfp_t)___GFP_FS)
#define __GFP_ZERO ((gfp_t)___GFP_ZERO)
#define __GFP_DIRECT_RECLAIM ((gfp_t)___GFP_DIRECT_RECLAIM)
#define __GFP_KSWAPD_RECLAIM ((gfp_t)___GFP_KSWAPD_RECLAIM)
#define __GFP_RECLAIM ((gfp_t)(___GFP_DIRECT_RECLAIM | ___GFP_KSWAPD_RECLAIM))
#define __GFP_WRITE ((gfp_t)___GFP_WRITE)
#define __GFP_NOWARN ((gfp_t)___GFP_NOWARN)
#define __GFP_RETRY_MAYFAIL ((gfp_t)___GFP_RETRY_MAYFAIL)
#define __GFP_NOFAIL ((gfp_t)___GFP_NOFAIL)
#define __GFP_NORETRY ((gfp_t)___GFP_NORETRY)
#define __GFP_MEMALLOC ((gfp_t)___GFP_MEMALLOC)
#define __GFP_COMP ((gfp_t)___GFP_COMP)
#define __GFP_NOMEMALLOC ((gfp_t)___GFP_NOMEMALLOC)
#define __GFP_HARDWALL ((gfp_t)___GFP_HARDWALL)
#define __GFP_THISNODE ((gfp_t)___GFP_THISNODE)
#define __GFP_ACCOUNT ((gfp_t)___GFP_ACCOUNT)
#define __GFP_ZEROTAGS ((gfp_t)___GFP_ZEROTAGS)
#define __GFP_NOLOCKDEP ((gfp_t)___GFP_NOLOCKDEP)
#define __GFP_RECLAIM_MASK (__GFP_RECLAIM | __GFP_IO | __GFP_FS)

#define GFP_ATOMIC (__GFP_HIGH | __GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_NOWAIT (__GFP_KSWAPD_RECLAIM | __GFP_NOWARN)
#define GFP_NOIO (__GFP_RECLAIM)
#define GFP_NOFS (__GFP_RECLAIM | __GFP_IO)
#define GFP_USER (__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA __GFP_DMA
#define GFP_DMA32 __GFP_DMA32
#define GFP_HIGHUSER (GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE (GFP_HIGHUSER | __GFP_MOVABLE)
#define GFP_TRANSHUGE_LIGHT ((GFP_HIGHUSER_MOVABLE | __GFP_COMP | __GFP_NOMEMALLOC | __GFP_NOWARN) & ~__GFP_RECLAIM)
#define GFP_TRANSHUGE (GFP_TRANSHUGE_LIGHT | __GFP_DIRECT_RECLAIM)

#endif
