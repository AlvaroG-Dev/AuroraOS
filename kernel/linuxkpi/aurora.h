/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Puente con el kernel de Aurora: los servicios que importa linuxkpi.kur.
 * La capa no incluye cabeceras de Aurora (chocarian con las de Linux); cada
 * funcion se declara aqui con un nombre aurora_* y la etiqueta asm del
 * simbolo real, que el cargador .kur resuelve contra el ksymtab del kernel
 * (kernel/ksymtab_exports.c). Las firmas deben coincidir con las de Aurora.
 */
#ifndef _LKPI_AURORA_H
#define _LKPI_AURORA_H

#include <linux/types.h>
#include <linux/stdarg.h>

/* klog.h: niveles KLOG_PANIC=0 ... KLOG_DEBUG=4 */
#define AURORA_KLOG_PANIC 0
#define AURORA_KLOG_ERR 1
#define AURORA_KLOG_WARN 2
#define AURORA_KLOG_INFO 3
#define AURORA_KLOG_DEBUG 4
void aurora_klog_vprintf(int level, const char *fmt, va_list ap) __asm__("klog_vprintf");
void aurora_klog_puts(int level, const char *s) __asm__("klog_puts");

/* heap.h (solo para metadatos de la capa, no para kmalloc de Linux) */
void *aurora_kmalloc(size_t size) __asm__("kmalloc");
void aurora_kfree(void *p) __asm__("kfree");

/* pmm.h: paginas fisicas contiguas (buddy); 0 si no hay memoria */
u64 aurora_pmm_alloc_pages(u64 count) __asm__("pmm_alloc_pages");
void aurora_pmm_free_pages(u64 phys, u64 count) __asm__("pmm_free_pages");
u64 aurora_pmm_total_pages(void) __asm__("pmm_total_pages");

/* paging.h */
#define AURORA_PHYS_MAP_BASE 0xFFFF800000000000UL
#define AURORA_PHYS_MAP_SIZE (512UL << 30)
#define AURORA_MMIO_MAP_BASE 0xFFFFC00000000000UL
#define AURORA_PTE_WRITABLE 0x002UL
#define AURORA_PTE_WRITETHRU 0x008UL
#define AURORA_PTE_NOCACHE 0x010UL
#define AURORA_PTE_GLOBAL 0x100UL
#define AURORA_PTE_NX (1UL << 63)
u64 aurora_paging_get_phys(u64 virt) __asm__("paging_get_phys");
int aurora_vmm_alloc_pages(u64 vaddr, u64 num_pages, u64 flags) __asm__("vmm_alloc_pages");
void aurora_vmm_free_pages(u64 vaddr, u64 num_pages) __asm__("vmm_free_pages");
void *aurora_mmio_map(u64 phys, u64 size, u64 flags) __asm__("mmio_map");
void aurora_mmio_unmap(u64 phys, u64 size) __asm__("mmio_unmap");

/* sched.h / timerfd.h */
u64 aurora_sched_get_ticks(void) __asm__("sched_get_ticks");
s64 aurora_kernel_realtime_ms(void) __asm__("kernel_realtime_ms");
void aurora_preempt_disable(void) __asm__("preempt_disable");
void aurora_preempt_enable(void) __asm__("preempt_enable");

/* uaccess.h: 0 si bien, -EFAULT si no (strncpy: longitud o -EFAULT) */
long aurora_copy_to_user(void *dst, const void *src, size_t n) __asm__("copy_to_user");
long aurora_copy_from_user(void *dst, const void *src, size_t n) __asm__("copy_from_user");
long aurora_strncpy_from_user(char *dst, const char *src, size_t max) __asm__("strncpy_from_user");

/* panic.h */
void aurora_panic_noctx(const char *fmt, ...) __asm__("panic_noctx") __attribute__((noreturn));

#endif
