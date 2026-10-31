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
u64 aurora_pmm_free_pages_count(void) __asm__("pmm_free_pages_count");

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

/* power.h: apagado y reinicio. action: 0 = apagar, 1 = reiniciar, 2 = parar.
 * Fases de notificador: 0 = preparar, 1 = dispositivos, 2 = final (sin dormir). */
#define AURORA_POWER_OFF 0
#define AURORA_POWER_RESTART 1
#define AURORA_POWER_HALT 2
#define AURORA_POWER_PHASE_PREPARE 0
#define AURORA_POWER_PHASE_DEVICES 1
#define AURORA_POWER_PHASE_FINAL 2
typedef void (*aurora_power_notifier_fn)(int action, int phase, void *ctx);
void aurora_power_request(int action, const char *reason) __asm__("power_request")
	__attribute__((noreturn));
int aurora_power_register_notifier(aurora_power_notifier_fn fn, void *ctx, int prio)
	__asm__("power_register_notifier");
int aurora_power_unregister_notifier(aurora_power_notifier_fn fn, void *ctx)
	__asm__("power_unregister_notifier");

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
int aurora_pci_ecam_available(void) __asm__("pci_ecam_available");
u32 aurora_pci_read_config_ext_dword(u8 bus, u8 slot, u8 func, u16 off)
	__asm__("pci_read_config_ext_dword");
u16 aurora_pci_find_ext_capability(u8 bus, u8 slot, u8 func, u16 id)
	__asm__("pci_find_ext_capability");
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

/*
 * L4 (K8): identidad de las tareas y senales. task_info_t de process.h.
 * Mascaras de senales de Aurora: bit `sig` (no sig - 1 como Linux).
 */
struct aurora_process;			/* process_t (opaco) */
struct aurora_task_info {
	u32 tid, tgid, uid, euid;
	struct aurora_process *proc;	/* NULL en hilos del kernel */
	int is_kernel;
	char comm[16];
};
_Static_assert(sizeof(struct aurora_task_info) == 48, "task_info_t de Aurora");
int aurora_task_info_get(struct aurora_task *t, struct aurora_task_info *out)
	__asm__("task_info_get");
u32 aurora_task_tid(struct aurora_task *t) __asm__("task_tid");
u64 aurora_signal_unblocked_of(struct aurora_task *t) __asm__("signal_unblocked_of");
#define AURORA_SIGKILL 9

/*
 * L4 (K8): VFS, fds y VMA de dispositivo. Replicas de vfs_ops_t,
 * vfs_dirent_t, vfs_fs_ops_t, vma_t y vma_dev_ops_t (vfs.h, pf.h); el
 * kernel comprueba tamanos y desplazamientos en ksymtab_exports.c.
 */
struct aurora_vfs_node;			/* vfs_node_t (opaco) */
struct aurora_fd;			/* file_descriptor_t (opaco) */
struct aurora_vma;
#define AURORA_VFS_FILE 0x01
#define AURORA_VFS_DIRECTORY 0x02
#define AURORA_VFS_CHARDEVICE 0x04
#define AURORA_S_IFDIR 0040000
#define AURORA_S_IFCHR 0020000
#define AURORA_S_IFREG 0100000
struct aurora_vfs_dirent {
	char name[128];
	u32 type;			/* AURORA_VFS_FILE / _DIRECTORY */
	u64 size;
};
_Static_assert(sizeof(struct aurora_vfs_dirent) == 144, "vfs_dirent_t de Aurora");
struct aurora_vfs_ops {
	s64 (*read)(struct aurora_vfs_node *n, u64 off, size_t size, void *buf);
	s64 (*write)(struct aurora_vfs_node *n, u64 off, size_t size, const void *buf);
	int (*open)(struct aurora_vfs_node *n, int flags);
	int (*close)(struct aurora_vfs_node *n);
	bool (*readable)(struct aurora_vfs_node *n);
	void *create, *mkdir, *unlink, *mknod, *truncate;
	int (*readdir)(struct aurora_vfs_node *dir, u64 index, struct aurora_vfs_dirent *out);
	void *rename, *readlink, *symlink;
	int (*poll)(struct aurora_vfs_node *n, short events);
	struct aurora_wait_queue *(*poll_wq)(struct aurora_vfs_node *n);
	s64 (*ioctl)(struct aurora_vfs_node *n, unsigned long req, u64 arg);
	void *utimes, *chmod, *chown, *link, *fsync, *mmap;
	int (*get_page)(struct aurora_vfs_node *n, u64 pgoff, u64 *phys);
	s64 (*read_fd)(struct aurora_fd *f, void *ubuf, size_t n, u64 *pos);
	s64 (*write_fd)(struct aurora_fd *f, const void *ubuf, size_t n, u64 *pos);
	s64 (*llseek)(struct aurora_fd *f, s64 off, int whence, u64 cur);
	int (*setfl)(struct aurora_fd *f, int newflags);
	int (*mmap_dev)(struct aurora_fd *f, struct aurora_process *proc, struct aurora_vma *vma,
			u64 prot, u64 flags);
};
_Static_assert(sizeof(struct aurora_vfs_ops) == 29 * 8, "vfs_ops_t de Aurora");
struct aurora_vfs_fs_ops {
	struct aurora_vfs_node *(*lookup)(void *fs_priv, const char *path);
	const char *name;
	void *statfs;
};
struct aurora_vma_dev_ops {
	int (*fault)(struct aurora_vma *vma, u64 addr, int write);
	int (*open)(struct aurora_vma *nv, struct aurora_vma *old);
	void (*close)(struct aurora_vma *vma);
};
struct aurora_vma {
	u64 start, end, flags;		/* flags: PTE de usuario (WRITABLE, NX...) */
	u32 type, pad;
	void *next, *file_fd;
	u64 file_offset;
	void *file_node;
	const struct aurora_vma_dev_ops *dev_ops;
	void *dev_priv;
	struct aurora_process *dev_proc;
	u32 dev_flags;			/* AURORA_VMA_DEV_* */
	u32 dev_cache;			/* AURORA_PTE_NOCACHE/WRITETHRU */
};
#define AURORA_VMA_DEV_DONTCOPY 0x1
#define AURORA_PTE_USER 0x004UL
#define AURORA_O_ASYNC 0x2000
#define AURORA_O_NONBLOCK 0x800
#define AURORA_O_CLOEXEC 0x80000

struct aurora_vfs_node *aurora_vfs_node_alloc(const char *name, u32 vflags, u32 mode, u32 rdev,
					      const struct aurora_vfs_ops *ops, void *priv,
					      u64 size) __asm__("vfs_node_alloc");
void aurora_vfs_node_ref(struct aurora_vfs_node *n) __asm__("vfs_node_ref");
void aurora_vfs_node_free(struct aurora_vfs_node *n) __asm__("vfs_node_free");
void *aurora_vfs_node_get_priv(struct aurora_vfs_node *n) __asm__("vfs_node_get_priv");
const struct aurora_vfs_ops *aurora_vfs_node_ops(struct aurora_vfs_node *n) __asm__("vfs_node_ops");
void aurora_vfs_node_set_priv(struct aurora_vfs_node *n, void *priv) __asm__("vfs_node_set_priv");
struct aurora_vfs_node *aurora_vfs_fd_node(struct aurora_fd *f) __asm__("vfs_fd_node");
int aurora_vfs_fd_flags(struct aurora_fd *f) __asm__("vfs_fd_flags");
struct aurora_fd *aurora_vfs_fd_get_current(int fd) __asm__("vfs_fd_get_current");
void aurora_vfs_fd_put_ref(struct aurora_fd *f) __asm__("vfs_fd_put_ref");
int aurora_vfs_fd_reserve(int cloexec) __asm__("vfs_fd_reserve");
int aurora_vfs_fd_install_node(int fd, struct aurora_vfs_node *n, int flags)
	__asm__("vfs_fd_install_node");
void aurora_vfs_fd_unreserve(int fd) __asm__("vfs_fd_unreserve");
int aurora_vfs_send_sigio(int owner, int sig) __asm__("vfs_send_sigio");
int aurora_vfs_fd_owner(struct aurora_fd *f) __asm__("vfs_fd_owner");
int aurora_vfs_mount(const char *path, const struct aurora_vfs_fs_ops *ops, void *fs_priv)
	__asm__("vfs_mount");
int aurora_vfs_umount(const char *path) __asm__("vfs_umount");
/* devtmpfs.h: /dev/<path>; rdev = (major << 8) | minor */
int aurora_devtmpfs_register(const char *path, u32 rdev, u32 mode, u32 uid, u32 gid,
			     const struct aurora_vfs_ops *ops, void *priv)
	__asm__("devtmpfs_register");
int aurora_devtmpfs_unregister(const char *path) __asm__("devtmpfs_unregister");
/* nl_uevent.h: uevent hacia los monitores de libudev; msg = "add@/devices/..\0K=V\0..." */
void aurora_nl_uevent_broadcast(const char *msg, size_t len) __asm__("nl_uevent_broadcast");

/* aur_bus.c: respaldo con el .aur omitido por aur.skip (4B) */
int aurora_aur_bus_launch_skipped(u8 bus, u8 slot, u8 func) __asm__("aur_bus_launch_skipped");

/* gpu_proxy.c / cmdline.c */
int aurora_gpup_get_framebuffer(u64 *phys, u64 *size) __asm__("gpup_get_framebuffer");
int aurora_cmdline_get(const char *key, char *out, size_t outlen) __asm__("cmdline_get");

/* kernel/drm: el backend nativo cede /dev/dri y /sys/class/drm al de Linux */
void aurora_drm_backend_claim(void) __asm__("drm_backend_claim");
void aurora_drm_backend_release(void) __asm__("drm_backend_release");
/* pf.h */
int aurora_vma_dev_map_page(struct aurora_vma *vma, u64 addr, u64 phys)
	__asm__("vma_dev_map_page");
void aurora_vma_dev_zap(struct aurora_vma *vma, u64 start, u64 end) __asm__("vma_dev_zap");
/* vmap.h */
#define AURORA_VMAP_BASE 0xFFFFD00000000000UL
#define AURORA_VMAP_SIZE (64UL << 30)
u64 aurora_vmap_area_alloc(u64 size) __asm__("vmap_area_alloc");
int aurora_paging_map_page(u64 virt, u64 phys, u64 flags) __asm__("paging_map_page");
void aurora_paging_invalidate_tlb_global(u64 virt) __asm__("paging_invalidate_tlb_global");
int aurora_vmap_area_map(u64 va, u64 phys, u64 flags) __asm__("vmap_area_map");
void aurora_vmap_area_free(u64 va) __asm__("vmap_area_free");
/* drm.h: dmabuf de Aurora */
struct aurora_vfs_node *aurora_dmabuf_node_new_ext(u32 npages, const u64 *pages,
						   void (*release)(void *), void *priv)
	__asm__("dmabuf_node_new_ext");
int aurora_dmabuf_node_pages(struct aurora_vfs_node *n, u32 *npages, const u64 **pages)
	__asm__("dmabuf_node_pages");
void *aurora_dmabuf_node_ext_priv(struct aurora_vfs_node *n, void (*release)(void *))
	__asm__("dmabuf_node_ext_priv");
/*
 * L5 (K7): fences de Aurora (drm_syncobj.c: un syncobj binario) y fence
 * implicito de los dmabuf. aurora_fence es opaco; el kernel comprueba
 * struct dmabuf_resv_ops con un _Static_assert.
 */
struct aurora_fence;
struct aurora_fence *aurora_fence_create(int signaled) __asm__("aur_fence_create");
void aurora_fence_get(struct aurora_fence *f) __asm__("aur_fence_get");
void aurora_fence_put(struct aurora_fence *f) __asm__("aur_fence_put");
void aurora_fence_signal(struct aurora_fence *f) __asm__("aur_fence_signal");
bool aurora_fence_signaled(struct aurora_fence *f) __asm__("aur_fence_signaled");
void aurora_fence_wake(void) __asm__("aur_fence_wake");
/* 0: encolado (fn se llama una vez); -EALREADY: ya senalado, fn no se llama */
int aurora_fence_watch(struct aurora_fence *f, void (*fn)(void *), void *data)
	__asm__("aur_fence_watch");
int aurora_fence_unwatch(struct aurora_fence *f, void (*fn)(void *), void *data)
	__asm__("aur_fence_unwatch");
int aurora_fence_from_fd(int fd, struct aurora_fence **out) __asm__("aur_fence_from_fd");
int aurora_fence_to_fd(struct aurora_fence *f, int cloexec, int *fd_out)
	__asm__("aur_fence_to_fd");
struct aurora_dmabuf_resv_ops {
	struct aurora_fence *(*export_fence)(void *priv, u32 flags);
	int (*import_fence)(void *priv, u32 flags, struct aurora_fence *f);
	int (*poll)(void *priv, short events);
};
int aurora_dmabuf_node_set_resv_ops(struct aurora_vfs_node *n, void (*release)(void *),
				    const struct aurora_dmabuf_resv_ops *ops)
	__asm__("dmabuf_node_set_resv_ops");
int aurora_dmabuf_node_export_fence(struct aurora_vfs_node *n, u32 flags,
				    struct aurora_fence **out)
	__asm__("dmabuf_node_export_fence");
int aurora_dmabuf_node_import_fence(struct aurora_vfs_node *n, u32 flags,
				    struct aurora_fence *f) __asm__("dmabuf_node_import_fence");
/* firmware.h de Aurora */
struct aurora_firmware {
	size_t size;
	const u8 *data;
};
int aurora_request_firmware(const struct aurora_firmware **fw, const char *name)
	__asm__("request_firmware");
void aurora_release_firmware(const struct aurora_firmware *fw) __asm__("release_firmware");

#endif
