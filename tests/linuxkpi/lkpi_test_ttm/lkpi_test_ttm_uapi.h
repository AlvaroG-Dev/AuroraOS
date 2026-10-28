/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Interfaz de /dev/lkpi_ttm (L6), comun al driver (test_ttm.c con KT_DEVICE=1)
 * y al programa de usuario (user/musl/apps/lkpi_test). Un dispositivo de
 * memoria simulado sobre ttm: VRAM de 4 MiB en RAM, GTT de paginas del
 * sistema y SYSTEM; los BO se mapean con ttm_bo_vm.
 */
#ifndef LKPI_TEST_TTM_UAPI_H
#define LKPI_TEST_TTM_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
#endif

#define LKPI_TTM_DEV "/dev/lkpi_ttm"

#define LKPI_TTM_VRAM_SIZE (4u << 20)

/* Donde puede vivir un BO */
#define LKPI_TTM_DOM_VRAM	1	/* solo VRAM (fuerza evicion si no cabe) */
#define LKPI_TTM_DOM_GTT	2	/* solo GTT */
#define LKPI_TTM_DOM_VRAM_GTT	3	/* VRAM y, si no hay, GTT */

struct lkpi_ttm_create {
	__u64 size;		/* in */
	__u32 domain;		/* in: LKPI_TTM_DOM_* */
	__u32 handle;		/* out */
	__u64 offset;		/* out: offset para mmap */
};

struct lkpi_ttm_move {
	__u32 handle;
	__u32 domain;		/* in: LKPI_TTM_DOM_VRAM o LKPI_TTM_DOM_GTT */
};

struct lkpi_ttm_info {
	__u32 handle;		/* in */
	__u32 mem_type;		/* out: 0 SYSTEM, 1 TT, 2 VRAM */
	__u32 usage_vram;	/* out: bytes en uso en VRAM */
	__u32 pad;
	__u64 size;		/* out */
};

/* El kernel rellena o comprueba el BO con un patron (kmap); bad = discrepancias */
struct lkpi_ttm_pattern {
	__u32 handle;
	__u32 seed;
	__s32 bad;		/* out (solo CHECK) */
	__u32 pad;
};

struct lkpi_ttm_pressure {
	__u32 count;		/* BO de 256 KiB solo-VRAM que retiene el fichero */
	__u32 pad;
};

#define LKPI_TTM_IOC_MAGIC 'T'
#define LKPI_TTM_IOC_CREATE _IOWR(LKPI_TTM_IOC_MAGIC, 1, struct lkpi_ttm_create)
#define LKPI_TTM_IOC_MOVE _IOW(LKPI_TTM_IOC_MAGIC, 2, struct lkpi_ttm_move)
#define LKPI_TTM_IOC_INFO _IOWR(LKPI_TTM_IOC_MAGIC, 3, struct lkpi_ttm_info)
#define LKPI_TTM_IOC_FILL _IOW(LKPI_TTM_IOC_MAGIC, 4, struct lkpi_ttm_pattern)
#define LKPI_TTM_IOC_CHECK _IOWR(LKPI_TTM_IOC_MAGIC, 5, struct lkpi_ttm_pattern)
#define LKPI_TTM_IOC_DESTROY _IOW(LKPI_TTM_IOC_MAGIC, 6, __u32)	/* handle */
#define LKPI_TTM_IOC_PRESSURE _IOW(LKPI_TTM_IOC_MAGIC, 7, struct lkpi_ttm_pressure)

/* Mismo patron que escribe el kernel en kt_bo_rw: palabra i (i multiplo de 61). */
#define LKPI_TTM_WORD(seed, pg16, i) \
	((__u32)((seed) ^ (__u32)((pg16) * 1031UL + (unsigned long)(i) * 2654435761U)))

#endif
