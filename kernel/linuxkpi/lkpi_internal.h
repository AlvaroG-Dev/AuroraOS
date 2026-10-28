/* SPDX-License-Identifier: GPL-2.0-only */
/* Declaraciones internas de linuxkpi.kur (no las ven los drivers). */
#ifndef _LKPI_INTERNAL_H
#define _LKPI_INTERNAL_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/cpumask.h>

int lkpi_mm_init(void);
void lkpi_mm_exit(void);
int lkpi_slab_init(void);
void lkpi_slab_exit(void);
void lkpi_vmalloc_exit(void);
void lkpi_ioremap_exit(void);

/* K2 (kernel/): arranque y parada, en este orden (y el inverso al salir). */
int lkpi_sched_init(void);
void lkpi_sched_exit(void);
void lkpi_time_init(void);
void lkpi_wait_init(void);
int lkpi_softirq_init(void);
void lkpi_softirq_exit(void);
int lkpi_rcu_init(void);
void lkpi_rcu_exit(void);
int lkpi_wq_init(void);
void lkpi_wq_exit(void);

/* K4/K5 (L3): modelo de dispositivos, plataforma y PCI, en este orden. */
int lkpi_kobject_init_layer(void);
void lkpi_kobject_exit_layer(void);
int lkpi_device_core_init(void);
void lkpi_device_core_exit(void);
int lkpi_platform_bus_init(void);
void lkpi_platform_bus_exit(void);
int lkpi_pci_init(void);
void lkpi_pci_exit(void);
struct device;
int lkpi_devres_release_all(struct device *dev);

/*
 * IRQ de la capa (kernel/irq.c). El "chip" lo pone pci/msi.c: mascara,
 * arranque/parada y destino. Las funciones del chip se llaman con
 * desc->lock tomado y pueden correr dentro de la IRQ.
 */
struct lkpi_irq_desc;
struct lkpi_irqaction;
struct lkpi_irq_chip {
	void (*mask)(struct lkpi_irq_desc *d);
	void (*unmask)(struct lkpi_irq_desc *d);
	void (*startup)(struct lkpi_irq_desc *d);
	void (*shutdown)(struct lkpi_irq_desc *d);
	int (*set_affinity)(struct lkpi_irq_desc *d, int cpu);
};
struct lkpi_irq_desc {
	unsigned int irq;		/* numero de Aurora: 0..15 o vector MSI */
	const struct lkpi_irq_chip *chip;
	void *chip_data;
	raw_spinlock_t lock;
	struct mutex request_mutex;
	struct lkpi_irqaction *action;	/* lista; se recorre sin cerrojo en la IRQ */
	int users;			/* INTx compartida entre dispositivos de la capa */
	int depth;			/* disable_irq anidados */
	bool masked;
	bool pending;			/* flanco llegado con la IRQ deshabilitada */
	bool level;			/* INTx: nivel */
	unsigned int threads_oneshot;	/* hilos ONESHOT en curso (linea enmascarada) */
	atomic_t in_progress;		/* entradas en curso (synchronize_irq) */
	atomic_t threads_active;
	wait_queue_head_t wait_for_threads;
	int cpu;			/* destino efectivo */
	struct cpumask affinity;
	u64 count;
	char name[16];
};
struct lkpi_irq_desc *lkpi_irq_desc_alloc(unsigned int irq, const struct lkpi_irq_chip *chip,
					  void *chip_data, int cpu, bool level);
struct lkpi_irq_desc *lkpi_irq_desc_get_shared(unsigned int irq);
void lkpi_irq_desc_free(struct lkpi_irq_desc *d);
u64 lkpi_irq_count(unsigned int irq);

struct pci_dev;
void lkpi_pci_msi_restore(struct pci_dev *dev);

/* Hilos internos de la capa (kernel/sched.c). */
struct task_struct;
struct task_struct *lkpi_kthread_create_internal(int (*fn)(void *), void *data,
						 const char *name);

/* Registro pfn -> struct page (mm/page.c). */
#define LKPI_PAGE_OWNED 0x1	/* el array salio de lkpi_alloc_pages */
#define LKPI_PAGE_FOREIGN 0x2	/* descriptor suelto de un marco ajeno */
struct page;
struct page *lkpi_page_lookup(unsigned long pfn);
struct page *lkpi_page_alloc_desc(unsigned long pfn);
bool lkpi_is_direct_map(const void *addr);

/* L4 (dma/): dma_buf. */
int lkpi_dmabuf_init(void);
void lkpi_dmabuf_exit(void);
/* L4 (misc/): i2c y backlight. */
int lkpi_i2c_init(void);
void lkpi_i2c_exit(void);
int lkpi_backlight_init(void);
void lkpi_backlight_exit(void);
void lkpi_pid_exit(void);
void lkpi_cpu_init(void);

/* dma/dma-buf.c: fd_install de un dma_buf */
bool lkpi_dma_buf_fd_install(unsigned int fd, struct file *file);

/* display-shadow (mm/ioremap.c, pci/pci.c) */
int lkpi_display_shadow_add(phys_addr_t start, size_t len, bool copy);
void lkpi_display_shadow_exit(void);
void lkpi_display_shadow_probe(void);

/* base/core.c: uevent de un dispositivo hacia netlink (lib/kobject.c) */
int lkpi_device_uevent_send(struct kobject *kobj, enum kobject_action action, char *extra[]);

#endif
