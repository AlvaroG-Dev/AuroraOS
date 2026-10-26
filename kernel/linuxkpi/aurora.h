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

/*
 * L3: modelo de dispositivos, PCI e interrupciones. Replicas de struct
 * device, struct driver (bus.h/driver.h) y pci_device_t (pci.h) de Aurora;
 * kernel/ksymtab_exports.c comprueba tamano y desplazamientos.
 */
struct aurora_bus_type;			/* struct bus_type (opaco) */
struct aurora_driver;
struct aurora_device {
	char name[16];			/* "0000:BB:SS.F" en PCI */
	struct aurora_bus_type *bus;
	void *priv;			/* struct aurora_pci_device * en PCI */
	const void *id;
	const void *matched_id;
	struct aurora_driver *driver;
	struct aurora_device *next;
};
_Static_assert(sizeof(struct aurora_device) == 64, "struct device de Aurora");
struct aurora_driver {
	const char *name;
	struct aurora_bus_type *bus;
	const void *id_table;		/* struct aurora_pci_device_id[], vendor 0 = fin */
	int (*probe)(struct aurora_device *dev);
	void (*remove)(struct aurora_device *dev);
	struct aurora_driver *next_on_bus;
	const char *const *depends;
	int (*init)(void);
	void (*shutdown)(void);
};
_Static_assert(sizeof(struct aurora_driver) == 72, "struct driver de Aurora");
struct aurora_pci_device_id {
	u16 vendor_id, device_id, subvendor, subdevice;
	u8 class_id, subclass_id, class_mask, pad;
};
#define AURORA_PCI_ANY_ID 0xFFFFu
struct aurora_pci_device {
	u8 bus, slot, func;
	u16 vendor_id, device_id, subsystem_vendor_id, subsystem_id;
	u8 class_id, subclass_id, prog_if, revision_id;
	u8 header_type, multifunction, irq_line, irq_pin;
	u32 bar[6];
	u8 bar_is_io[6];
	u8 bar_is_64[6];
	u8 secondary_bus, subordinate_bus;
};
_Static_assert(sizeof(struct aurora_pci_device) == 60, "pci_device_t de Aurora");
extern struct aurora_bus_type aurora_pci_bus __asm__("pci_bus");
int aurora_bus_add_driver(struct aurora_driver *drv) __asm__("bus_add_driver");
void aurora_bus_del_driver(struct aurora_driver *drv) __asm__("bus_del_driver");
const struct aurora_pci_device *aurora_pci_get_devices(int *count) __asm__("pci_get_devices");
u8 aurora_pci_read_config_byte(u8 bus, u8 slot, u8 func, u8 off) __asm__("pci_read_config_byte");
u16 aurora_pci_read_config_word(u8 bus, u8 slot, u8 func, u8 off) __asm__("pci_read_config_word");
u32 aurora_pci_read_config_dword(u8 bus, u8 slot, u8 func, u8 off)
	__asm__("pci_read_config_dword");
void aurora_pci_write_config_byte(u8 bus, u8 slot, u8 func, u8 off, u8 v)
	__asm__("pci_write_config_byte");
void aurora_pci_write_config_word(u8 bus, u8 slot, u8 func, u8 off, u16 v)
	__asm__("pci_write_config_word");
void aurora_pci_write_config_dword(u8 bus, u8 slot, u8 func, u8 off, u32 v)
	__asm__("pci_write_config_dword");

/* idt.h: vectores MSI de kernel (0x63..0x67, 0x70..0x7F); -1 si no quedan */
#define AURORA_MSI_VECTOR_BASE 0x60
#define AURORA_MSI_VECTOR_END 0x7F
int aurora_msi_vector_alloc(void) __asm__("msi_vector_alloc");
void aurora_msi_vector_free(u8 vector) __asm__("msi_vector_free");
/* irq_threaded.h: irq 0..15 = INTx (IOAPIC), 0x60..0x7F = vector MSI. El
 * ack corre en la IRQ (IF=0) antes del EOI. irqreturn_t: 0 NONE, 1 HANDLED,
 * 2 WAKE_THREAD (los mismos valores que Linux). */
typedef int (*aurora_irq_handler_fn)(int irq, void *dev_id);
int aurora_irq_request_threaded(unsigned int irq, aurora_irq_handler_fn ack,
				aurora_irq_handler_fn thread_fn, const char *name, void *dev_id)
	__asm__("irq_request_threaded");
void aurora_irq_free(unsigned int irq) __asm__("irq_free");
/* apic.h */
/* INTx de PCI: flags del ISO de la MADT si existe; si no, nivel/activa baja */
void aurora_ioapic_route_pci_intx(u8 irq, u8 vector, u32 dest_apic_id, int masked)
	__asm__("ioapic_route_pci_intx");
void aurora_ioapic_mask_irq(u8 irq, int masked) __asm__("ioapic_mask_irq");
/* smp_boot.h: -1 si la CPU no existe */
int aurora_smp_cpu_lapic_id(int cpu) __asm__("smp_cpu_lapic_id");
/* pmm.h: paginas contiguas por debajo de `limit` (alineadas a la potencia de
 * dos de count); se liberan con pmm_free_pages. 0 si no hay. */
u64 aurora_pmm_alloc_pages_below(u64 count, u64 limit) __asm__("pmm_alloc_pages_below");

/* sysfs_dyn.h: nodos de /sys (rutas relativas, sin "/sys/") */
struct aurora_sysfs_attr_ops {
	long (*show)(void *ctx, char *buf, size_t size);
	long (*store)(void *ctx, const char *buf, size_t len);
};
#define AURORA_SYSFS_PATH_MAX 192
#define AURORA_SYSFS_ATTR_BUF 4096
int aurora_sysfs_set_file(const char *path, const char *content) __asm__("sysfs_dyn_set_file");
int aurora_sysfs_add_symlink(const char *path, const char *target)
	__asm__("sysfs_dyn_add_symlink");
int aurora_sysfs_add_attr(const char *path, const struct aurora_sysfs_attr_ops *ops,
			  void *ctx, u32 mode) __asm__("sysfs_dyn_add_attr");
int aurora_sysfs_remove(const char *path) __asm__("sysfs_dyn_remove");
int aurora_sysfs_remove_tree(const char *prefix) __asm__("sysfs_dyn_remove_tree");

#endif
