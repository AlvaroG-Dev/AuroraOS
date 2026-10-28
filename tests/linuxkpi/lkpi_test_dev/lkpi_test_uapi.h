/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Interfaz de /dev/lkpi_test (hito de L4), comun al driver
 * (test_dev.c, contra las cabeceras de la capa) y al programa de
 * usuario (user/musl/apps/lkpi_test, contra musl). Solo tipos de tamano fijo.
 */
#ifndef LKPI_TEST_UAPI_H
#define LKPI_TEST_UAPI_H

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

#define LKPI_TEST_DEV "/dev/lkpi_test"
#define LKPI_TEST_MEM_SIZE 4096		/* read/write/llseek sobre esta memoria */
#define LKPI_TEST_DMA_SIZE (4 * 4096)	/* mmap offset 0: memoria coherente */
#define LKPI_TEST_FAULT_PGOFF 256	/* mmap offset 1 MiB: paginas por fallo */
#define LKPI_TEST_FAULT_PAGES 4
#define LKPI_TEST_DMABUF_PAGES 4

struct lkpi_test_info {
	__s32 pid, tgid;		/* current visto por el driver */
	__u32 uid, has_mm;
	char comm[16];
	__u32 opens, releases;		/* del fichero de dispositivo */
	__u32 vma_opens, vma_closes, faults;
	__u32 events;			/* eventos generados (RAISE) */
	__u64 dma_handle;		/* direccion de bus de la memoria coherente */
};

struct lkpi_test_fw {
	char name[64];			/* in: fichero bajo /lib/firmware */
	__s32 result;			/* out: 0 o -errno */
	__u32 size, crc32;		/* out: tamano y CRC-32 (como zlib) */
};

struct lkpi_test_wait {
	__u32 events;			/* out: eventos vistos */
	__s32 interrupted;		/* out: 1 si una senal corto la espera */
};

struct lkpi_test_dmabuf {
	__s32 fd;			/* EXPORT: out; IMPORT: in */
	__u32 seed;			/* EXPORT: in (patron); IMPORT: out */
	__u32 size;			/* out */
	__u32 sum;			/* IMPORT: out, suma de los bytes leidos por vmap */
	__u32 nents;			/* IMPORT: out, entradas de la sg_table */
	__u32 pad;
};

/* K7: fence implicito de un dma_buf exportado */
#define LKPI_FENCE_ATTACH 1	/* fd: dma_buf; pone un fence de escritura pendiente en su resv */
#define LKPI_FENCE_SIGNAL 2	/* senala el fence puesto */
struct lkpi_test_fence {
	__s32 fd;
	__u32 op;
};

#define LKPI_TEST_IOC_MAGIC 'L'
#define LKPI_IOC_INFO _IOR(LKPI_TEST_IOC_MAGIC, 1, struct lkpi_test_info)
#define LKPI_IOC_FIRMWARE _IOWR(LKPI_TEST_IOC_MAGIC, 2, struct lkpi_test_fw)
#define LKPI_IOC_RAISE_LATER _IOW(LKPI_TEST_IOC_MAGIC, 3, __u32)	/* ms */
#define LKPI_IOC_WAIT _IOR(LKPI_TEST_IOC_MAGIC, 4, struct lkpi_test_wait)
#define LKPI_IOC_DMABUF_EXPORT _IOWR(LKPI_TEST_IOC_MAGIC, 5, struct lkpi_test_dmabuf)
#define LKPI_IOC_DMABUF_IMPORT _IOWR(LKPI_TEST_IOC_MAGIC, 6, struct lkpi_test_dmabuf)
#define LKPI_IOC_ZAP _IO(LKPI_TEST_IOC_MAGIC, 7)	/* unmap_mapping_range */
#define LKPI_IOC_FENCE _IOW(LKPI_TEST_IOC_MAGIC, 8, struct lkpi_test_fence)

/* Patron de los buffers: byte i = seed + i * 7. */
#define LKPI_TEST_PATTERN(seed, i) ((unsigned char)((seed) + (i) * 7))

#endif
