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

/*
 * L2: aparcar/despertar tareas. Replica de wait_queue_t (wait.h; el kernel
 * comprueba el tamano con un _Static_assert en ksymtab_exports.c). La capa
 * solo la reserva, la inicializa y le pasa punteros; no toca sus campos.
 */
struct aurora_task;			/* task_t (opaco) */
struct aurora_wait_queue {
	u32 lock;			/* spinlock_t */
	void *head;
	int nr_waiting;
	void *subs;
};
_Static_assert(sizeof(struct aurora_wait_queue) == 32, "wait_queue_t de Aurora");
struct aurora_task *aurora_sched_current(void) __asm__("sched_current");
void aurora_sched_yield(void) __asm__("sched_yield");
void aurora_sched_set_blocked_deadline(struct aurora_task *t, u64 deadline)
	__asm__("sched_set_blocked_deadline");
u64 aurora_sched_rcu_qs_count(int cpu) __asm__("sched_rcu_qs_count");
int aurora_preempt_count(void) __asm__("preempt_count");
void aurora_wait_queue_init(struct aurora_wait_queue *wq) __asm__("wait_queue_init");
void aurora_wait_queue_add_locked(struct aurora_wait_queue *wq, struct aurora_task *t)
	__asm__("wait_queue_add_locked");
void aurora_wait_queue_remove_locked(struct aurora_wait_queue *wq, struct aurora_task *t)
	__asm__("wait_queue_remove_locked");
void aurora_wake_up_all_locked(struct aurora_wait_queue *wq) __asm__("wake_up_all_locked");
unsigned long aurora_spin_lock_irqsave(u32 *lock) __asm__("spin_lock_irqsave");
void aurora_spin_unlock_irqrestore(u32 *lock, unsigned long flags)
	__asm__("spin_unlock_irqrestore");

/* kthread.h: hilos del kernel (handle opaco struct kthread) */
struct aurora_kthread;
struct aurora_kthread *aurora_kthread_create(int (*fn)(void *), void *data, const char *name)
	__asm__("kthread_create");
int aurora_kthread_start(struct aurora_kthread *kt) __asm__("kthread_start");
int aurora_kthread_stop(struct aurora_kthread *kt) __asm__("kthread_stop");
struct aurora_task *aurora_kthread_task(struct aurora_kthread *kt) __asm__("kthread_task");

/* klog.h / smp_boot.h / cpu.h */
u64 aurora_klog_get_tsc_freq(void) __asm__("klog_get_tsc_freq");
int aurora_smp_online_cpus(void) __asm__("smp_online_cpus");
#define AURORA_MAX_CPUS 64
/* cpu.h: smp_processor_id() lee cpu_local_t.cpu_id en %gs:16 (CPU_LOCAL_CPU_ID_OFFSET). */
#define AURORA_CPU_LOCAL_CPU_ID_OFFSET 16

/* uaccess.h: 0 si bien, -EFAULT si no (strncpy: longitud o -EFAULT) */
long aurora_copy_to_user(void *dst, const void *src, size_t n) __asm__("copy_to_user");
long aurora_copy_from_user(void *dst, const void *src, size_t n) __asm__("copy_from_user");
long aurora_strncpy_from_user(char *dst, const char *src, size_t max) __asm__("strncpy_from_user");

/* panic.h */
void aurora_panic_noctx(const char *fmt, ...) __asm__("panic_noctx") __attribute__((noreturn));

#endif
