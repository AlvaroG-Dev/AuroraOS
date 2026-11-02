/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Modelo de dispositivos de Linux (K4): struct device, bus_type,
 * device_driver, class, devres/devm_* y dev_printk. Implementacion en
 * kernel/linuxkpi/drivers/base/. Los dispositivos registrados aparecen en el
 * /sys real de Aurora (devices/..., bus/<bus>/..., class/<clase>/...).
 *
 * Diferencias con Linux:
 *  - Un unico cerrojo global para las listas de buses, drivers y clases; el
 *    probe y el remove se llaman con device_lock(dev) y sin ese cerrojo.
 *  - Sin probe asincrono, sin device links ni gestion de energia (L5/L9).
 *  - Los uevents no salen al userland todavia (L5).
 */
#ifndef _LKPI_LINUX_DEVICE_H
#define _LKPI_LINUX_DEVICE_H

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/pm.h>
#include <linux/ioport.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/stdarg.h>
#include <linux/kdev_t.h>
#include <linux/cleanup.h>

struct device;
struct device_driver;
struct device_node;
struct fwnode_handle;
struct of_device_id;
struct acpi_device_id;
struct iommu_group;
struct dev_pin_info;
struct bus_dma_region;
struct device_dma_parameters {
	unsigned int max_segment_size;
	unsigned int min_align_mask;
	unsigned long segment_boundary_mask;
};

/* ------------------------------------------------------------ atributos */

struct device_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
};

#define DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_PREALLOC(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = __ATTR_PREALLOC(_name, _mode, _show, _store)
#define DEVICE_ATTR_RW(_name) struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define DEVICE_ATTR_ADMIN_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW_MODE(_name, 0600)
#define DEVICE_ATTR_RO(_name) struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_ADMIN_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO_MODE(_name, 0400)
#define DEVICE_ATTR_WO(_name) struct device_attribute dev_attr_##_name = __ATTR_WO(_name)

struct dev_ext_attribute {
	struct device_attribute attr;
	void *var;
};
ssize_t lkpi_device_show_ulong(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t lkpi_device_store_ulong(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t size);
ssize_t lkpi_device_show_int(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t lkpi_device_store_int(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size);
ssize_t lkpi_device_show_bool(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t lkpi_device_store_bool(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t size);
#define device_show_ulong lkpi_device_show_ulong
#define device_store_ulong lkpi_device_store_ulong
#define device_show_int lkpi_device_show_int
#define device_store_int lkpi_device_store_int
#define device_show_bool lkpi_device_show_bool
#define device_store_bool lkpi_device_store_bool
#define DEVICE_ULONG_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_ulong, device_store_ulong), &(_var) }
#define DEVICE_INT_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_int, device_store_int), &(_var) }
#define DEVICE_BOOL_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_bool, device_store_bool), &(_var) }

/* ---------------------------------------------------------------- tipos */

enum probe_type {
	PROBE_DEFAULT_STRATEGY,
	PROBE_PREFER_ASYNCHRONOUS,
	PROBE_FORCE_SYNCHRONOUS,
};

struct bus_type {
	const char *name;
	const char *dev_name;
	const struct attribute_group **bus_groups;
	const struct attribute_group **dev_groups;
	const struct attribute_group **drv_groups;

	int (*match)(struct device *dev, const struct device_driver *drv);
	int (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
	int (*probe)(struct device *dev);
	void (*sync_state)(struct device *dev);
	void (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
	const struct cpumask *(*irq_get_affinity)(struct device *dev, unsigned int irq_vec);

	int (*online)(struct device *dev);
	int (*offline)(struct device *dev);
	int (*suspend)(struct device *dev, pm_message_t state);
	int (*resume)(struct device *dev);
	int (*num_vf)(struct device *dev);
	int (*dma_configure)(struct device *dev);
	void (*dma_cleanup)(struct device *dev);

	const struct dev_pm_ops *pm;
	bool driver_override;
	bool need_parent_lock;
};

struct driver_private;		/* de la capa */

struct device_driver {
	const char *name;
	const struct bus_type *bus;
	struct module *owner;
	const char *mod_name;
	bool suppress_bind_attrs;
	enum probe_type probe_type;
	const struct of_device_id *of_match_table;
	const struct acpi_device_id *acpi_match_table;

	int (*probe)(struct device *dev);
	void (*sync_state)(struct device *dev);
	int (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
	int (*suspend)(struct device *dev, pm_message_t state);
	int (*resume)(struct device *dev);
	const struct attribute_group **groups;
	const struct attribute_group **dev_groups;
	const struct dev_pm_ops *pm;
	void (*coredump)(struct device *dev);

	struct driver_private *p;
};

struct class {
	const char *name;
	const struct attribute_group **class_groups;
	const struct attribute_group **dev_groups;
	int (*dev_uevent)(const struct device *dev, struct kobj_uevent_env *env);
	char *(*devnode)(const struct device *dev, umode_t *mode);
	void (*class_release)(const struct class *class);
	void (*dev_release)(struct device *dev);
	int (*shutdown_pre)(struct device *dev);
	const struct kobj_ns_type_operations *ns_type;
	const void *(*namespace)(const struct device *dev);
	void (*get_ownership)(const struct device *dev, kuid_t *uid, kgid_t *gid);
	const struct dev_pm_ops *pm;
};

struct device_type {
	const char *name;
	const struct attribute_group **groups;
	int (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
	char *(*devnode)(const struct device *dev, umode_t *mode, kuid_t *uid, kgid_t *gid);
	void (*release)(struct device *dev);
	const struct dev_pm_ops *pm;
};

struct device {
	struct kobject kobj;
	struct device *parent;
	void *p;			/* sin uso (en Linux, device_private) */
	const char *init_name;
	const struct device_type *type;
	const struct bus_type *bus;
	struct device_driver *driver;
	void *platform_data;
	void *driver_data;
	struct mutex mutex;		/* device_lock() */
	struct dev_pm_info power;
	struct dev_pm_domain *pm_domain;
	u64 *dma_mask;
	u64 coherent_dma_mask;
	u64 bus_dma_limit;
	const struct bus_dma_region *dma_range_map;
	struct device_dma_parameters *dma_parms;
	struct list_head dma_pools;
	struct device_node *of_node;
	struct fwnode_handle *fwnode;
	int numa_node;
	dev_t devt;
	u32 id;
	spinlock_t devres_lock;
	struct list_head devres_head;
	const struct class *class;
	const struct attribute_group **groups;
	void (*release)(struct device *dev);
	struct iommu_group *iommu_group;
	bool offline_disabled:1;
	bool offline:1;
	bool of_node_reused:1;
	bool state_synced:1;
	bool can_match:1;
	bool dma_coherent:1;
	bool dma_skip_sync:1;
	bool dma_iommu:1;
	/* capa: listas de la capa (bus, driver, clase, hijos) */
	struct list_head lkpi_bus_node;
	struct list_head lkpi_drv_node;
	struct list_head lkpi_class_node;
	struct list_head lkpi_children;
	struct list_head lkpi_child_node;
	unsigned int lkpi_probe_gen;
	bool lkpi_deferred;
	char *lkpi_class_link;		/* class/<clase>/<nombre> */
	char *lkpi_bus_link;		/* bus/<bus>/devices/<nombre> */
};

#define kobj_to_dev(__kobj) container_of_const(__kobj, struct device, kobj)
#define to_device_private_bus(p) NULL

static inline const char *dev_name(const struct device *dev)
{
	if (dev->init_name)
		return dev->init_name;
	return kobject_name(&dev->kobj);
}
static inline const char *dev_bus_name(const struct device *dev)
{
	return dev->bus ? dev->bus->name : (dev->class ? dev->class->name : NULL);
}
static inline void *dev_get_drvdata(const struct device *dev) { return dev->driver_data; }
static inline void dev_set_drvdata(struct device *dev, void *data) { dev->driver_data = data; }
static inline void *dev_get_platdata(const struct device *dev) { return dev->platform_data; }
static inline void set_dev_node(struct device *dev, int node) { dev->numa_node = node; }
static inline struct device_node *dev_of_node(struct device *dev) { return NULL; }
static inline struct fwnode_handle *dev_fwnode(const struct device *dev) { return dev->fwnode; }
static inline bool device_is_registered(struct device *dev) { return dev->kobj.state_in_sysfs; }
static inline bool device_is_bound(struct device *dev) { return dev->driver != NULL; }
static inline void device_lock(struct device *dev) { mutex_lock(&dev->mutex); }
static inline int device_lock_interruptible(struct device *dev)
{
	return mutex_lock_interruptible(&dev->mutex);
}
static inline int device_trylock(struct device *dev) { return mutex_trylock(&dev->mutex); }
static inline void device_unlock(struct device *dev) { mutex_unlock(&dev->mutex); }
static inline void device_lock_assert(struct device *dev) { }
DEFINE_GUARD(device, struct device *, device_lock(_T), device_unlock(_T))
static inline void dev_set_removable(struct device *dev, int removable) { }
static inline void device_enable_async_suspend(struct device *dev) { }
static inline void device_disable_async_suspend(struct device *dev) { }
static inline bool dev_has_sync_state(struct device *dev) { return false; }
static inline void dev_set_msi_domain(struct device *dev, void *d) { }
static inline int dev_num_vf(struct device *dev) { return 0; }

static inline struct device *kobj_to_dev_nonconst(struct kobject *kobj)
{
	return container_of(kobj, struct device, kobj);
}

/* -------------------------------------------------------------- nucleo */

void lkpi_device_initialize(struct device *dev);
int lkpi_device_add(struct device *dev);
int lkpi_device_register(struct device *dev);
void lkpi_device_del(struct device *dev);
void lkpi_device_unregister(struct device *dev);
struct device *lkpi_get_device(struct device *dev);
void lkpi_put_device(struct device *dev);
__printf(2, 3) int lkpi_dev_set_name(struct device *dev, const char *name, ...);
int lkpi_device_rename(struct device *dev, const char *new_name);
const char *lkpi_dev_driver_string(const struct device *dev);
int lkpi_device_for_each_child(struct device *parent, void *data,
			       int (*fn)(struct device *dev, void *data));
int lkpi_device_for_each_child_reverse(struct device *parent, void *data,
				       int (*fn)(struct device *dev, void *data));
struct device *lkpi_device_find_child(struct device *parent, const void *data,
				      int (*match)(struct device *dev, const void *data));
struct device *lkpi_device_find_child_by_name(struct device *parent, const char *name);
int lkpi_device_create_file(struct device *dev, const struct device_attribute *attr);
void lkpi_device_remove_file(struct device *dev, const struct device_attribute *attr);
bool lkpi_device_remove_file_self(struct device *dev, const struct device_attribute *attr);
int lkpi_device_create_bin_file(struct device *dev, const struct bin_attribute *attr);
void lkpi_device_remove_bin_file(struct device *dev, const struct bin_attribute *attr);
int lkpi_device_add_groups(struct device *dev, const struct attribute_group **groups);
void lkpi_device_remove_groups(struct device *dev, const struct attribute_group **groups);
int lkpi_devm_device_add_group(struct device *dev, const struct attribute_group *grp);
int lkpi_device_attach(struct device *dev);
int lkpi_device_driver_attach(const struct device_driver *drv, struct device *dev);
void lkpi_device_release_driver(struct device *dev);
int lkpi_device_reprobe(struct device *dev);
int lkpi_driver_attach(const struct device_driver *drv);
void lkpi_driver_deferred_probe_check_state(struct device *dev);
__printf(3, 4) int lkpi_dev_err_probe(const struct device *dev, int err, const char *fmt, ...);
struct device *lkpi_root_device_register(const char *name);
void lkpi_root_device_unregister(struct device *root);
struct device *lkpi_device_create(const struct class *cls, struct device *parent, dev_t devt,
				  void *drvdata, const char *fmt, ...) __printf(5, 6);
struct device *lkpi_device_create_with_groups(const struct class *cls, struct device *parent,
					      dev_t devt, void *drvdata,
					      const struct attribute_group **groups,
					      const char *fmt, ...) __printf(6, 7);
void lkpi_device_destroy(const struct class *cls, dev_t devt);

#define device_initialize(d) lkpi_device_initialize(d)
#define device_add(d) lkpi_device_add(d)
#define device_register(d) lkpi_device_register(d)
#define device_del(d) lkpi_device_del(d)
#define device_unregister(d) lkpi_device_unregister(d)
#define get_device(d) lkpi_get_device(d)
#define put_device(d) lkpi_put_device(d)
#define dev_set_name lkpi_dev_set_name
#define device_rename(d, n) lkpi_device_rename(d, n)
#define dev_driver_string(d) lkpi_dev_driver_string(d)
#define device_for_each_child(p, d, f) lkpi_device_for_each_child(p, d, f)
#define device_for_each_child_reverse(p, d, f) lkpi_device_for_each_child_reverse(p, d, f)
#define device_find_child(p, d, m) lkpi_device_find_child(p, d, m)
#define device_find_child_by_name(p, n) lkpi_device_find_child_by_name(p, n)
#define device_create_file(d, a) lkpi_device_create_file(d, a)
#define device_remove_file(d, a) lkpi_device_remove_file(d, a)
#define device_remove_file_self(d, a) lkpi_device_remove_file_self(d, a)
#define device_create_bin_file(d, a) lkpi_device_create_bin_file(d, a)
#define device_remove_bin_file(d, a) lkpi_device_remove_bin_file(d, a)
#define device_add_groups(d, g) lkpi_device_add_groups(d, g)
#define device_remove_groups(d, g) lkpi_device_remove_groups(d, g)
#define devm_device_add_group(d, g) lkpi_devm_device_add_group(d, g)
static inline int device_attach(struct device *d) { return lkpi_device_attach(d); }
#define device_driver_attach(drv, d) lkpi_device_driver_attach(drv, d)
#define device_release_driver(d) lkpi_device_release_driver(d)
#define device_reprobe(d) lkpi_device_reprobe(d)
#define driver_attach(drv) lkpi_driver_attach(drv)
#define dev_err_probe lkpi_dev_err_probe
#define root_device_register(n) lkpi_root_device_register(n)
#define root_device_unregister(r) lkpi_root_device_unregister(r)
#define device_create lkpi_device_create
#define device_create_with_groups lkpi_device_create_with_groups
#define device_destroy(c, d) lkpi_device_destroy(c, d)

static inline int device_add_group(struct device *dev, const struct attribute_group *grp)
{
	const struct attribute_group *groups[] = { grp, NULL };
	return device_add_groups(dev, groups);
}
static inline void device_remove_group(struct device *dev, const struct attribute_group *grp)
{
	const struct attribute_group *groups[] = { grp, NULL };
	device_remove_groups(dev, groups);
}
DEFINE_FREE(put_device, struct device *, if (_T) put_device(_T))

/* -------------------------------------------------------- device_link */

/*
 * Enlaces proveedor -> consumidor. MINIMO: se registran (con referencia a los
 * dos dispositivos) pero no ordenan el probe, el remove ni la energia, que es
 * para lo que los usa Linux. Basta para drivers que los crean y los borran
 * (simpledrm con varios dominios de energia, que en x86 no hay).
 */
#define DL_FLAG_STATELESS BIT(0)
#define DL_FLAG_AUTOREMOVE_CONSUMER BIT(1)
#define DL_FLAG_PM_RUNTIME BIT(2)
#define DL_FLAG_RPM_ACTIVE BIT(3)
#define DL_FLAG_AUTOREMOVE_SUPPLIER BIT(4)
#define DL_FLAG_AUTOPROBE_CONSUMER BIT(5)
#define DL_FLAG_MANAGED BIT(6)
#define DL_FLAG_SYNC_STATE_ONLY BIT(7)
#define DL_FLAG_INFERRED BIT(8)
#define DL_FLAG_CYCLE BIT(9)

struct device_link {
	struct device *supplier;
	struct device *consumer;
	u32 flags;
};

struct device_link *lkpi_device_link_add(struct device *consumer, struct device *supplier,
					 u32 flags);
void lkpi_device_link_del(struct device_link *link);
#define device_link_add(c, s, f) lkpi_device_link_add(c, s, f)
#define device_link_del(l) lkpi_device_link_del(l)

/* ----------------------------------------------------------------- bus */

struct bus_attribute {
	struct attribute attr;
	ssize_t (*show)(const struct bus_type *bus, char *buf);
	ssize_t (*store)(const struct bus_type *bus, const char *buf, size_t count);
};
#define BUS_ATTR_RW(_name) struct bus_attribute bus_attr_##_name = __ATTR_RW(_name)
#define BUS_ATTR_RO(_name) struct bus_attribute bus_attr_##_name = __ATTR_RO(_name)
#define BUS_ATTR_WO(_name) struct bus_attribute bus_attr_##_name = __ATTR_WO(_name)

#define BUS_NOTIFY_ADD_DEVICE 0x00000001
#define BUS_NOTIFY_DEL_DEVICE 0x00000002
#define BUS_NOTIFY_REMOVED_DEVICE 0x00000003
#define BUS_NOTIFY_BIND_DRIVER 0x00000004
#define BUS_NOTIFY_BOUND_DRIVER 0x00000005
#define BUS_NOTIFY_UNBIND_DRIVER 0x00000006
#define BUS_NOTIFY_UNBOUND_DRIVER 0x00000007
#define BUS_NOTIFY_DRIVER_NOT_BOUND 0x00000008

struct notifier_block;
int lkpi_bus_register(const struct bus_type *bus);
void lkpi_bus_unregister(const struct bus_type *bus);
int lkpi_bus_rescan_devices(const struct bus_type *bus);
int lkpi_bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data,
			  int (*fn)(struct device *dev, void *data));
struct device *lkpi_bus_find_device(const struct bus_type *bus, struct device *start,
				    const void *data,
				    int (*match)(struct device *dev, const void *data));
int lkpi_bus_for_each_drv(const struct bus_type *bus, struct device_driver *start, void *data,
			  int (*fn)(struct device_driver *drv, void *data));
int lkpi_bus_register_notifier(const struct bus_type *bus, struct notifier_block *nb);
int lkpi_bus_unregister_notifier(const struct bus_type *bus, struct notifier_block *nb);
int lkpi_bus_create_file(const struct bus_type *bus, struct bus_attribute *attr);
void lkpi_bus_remove_file(const struct bus_type *bus, struct bus_attribute *attr);
int lkpi_device_match_name(struct device *dev, const void *name);
int lkpi_device_match_devt(struct device *dev, const void *pdevt);
int lkpi_device_match_any(struct device *dev, const void *unused);

#define bus_register(b) lkpi_bus_register(b)
#define bus_unregister(b) lkpi_bus_unregister(b)
#define bus_rescan_devices(b) lkpi_bus_rescan_devices(b)
#define bus_for_each_dev(b, s, d, f) lkpi_bus_for_each_dev(b, s, d, f)
#define bus_find_device(b, s, d, m) lkpi_bus_find_device(b, s, d, m)
#define bus_for_each_drv(b, s, d, f) lkpi_bus_for_each_drv(b, s, d, f)
#define bus_register_notifier(b, n) lkpi_bus_register_notifier(b, n)
#define bus_unregister_notifier(b, n) lkpi_bus_unregister_notifier(b, n)
#define bus_create_file(b, a) lkpi_bus_create_file(b, a)
#define bus_remove_file(b, a) lkpi_bus_remove_file(b, a)
#define device_match_name lkpi_device_match_name
#define device_match_devt lkpi_device_match_devt
#define device_match_any lkpi_device_match_any

static inline struct device *bus_find_device_by_name(const struct bus_type *bus,
						     struct device *start, const char *name)
{
	return bus_find_device(bus, start, name, device_match_name);
}
static inline struct device *bus_find_next_device(const struct bus_type *bus, struct device *cur)
{
	return bus_find_device(bus, cur, NULL, device_match_any);
}

/* -------------------------------------------------------------- driver */

struct driver_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device_driver *driver, char *buf);
	ssize_t (*store)(struct device_driver *driver, const char *buf, size_t count);
};
#define DRIVER_ATTR_RW(_name) struct driver_attribute driver_attr_##_name = __ATTR_RW(_name)
#define DRIVER_ATTR_RO(_name) struct driver_attribute driver_attr_##_name = __ATTR_RO(_name)
#define DRIVER_ATTR_WO(_name) struct driver_attribute driver_attr_##_name = __ATTR_WO(_name)

int lkpi_driver_register(struct device_driver *drv);
void lkpi_driver_unregister(struct device_driver *drv);
struct device_driver *lkpi_driver_find(const char *name, const struct bus_type *bus);
int lkpi_driver_for_each_device(struct device_driver *drv, struct device *start, void *data,
				int (*fn)(struct device *dev, void *));
struct device *lkpi_driver_find_device(const struct device_driver *drv, struct device *start,
				       const void *data,
				       int (*match)(struct device *dev, const void *data));
int lkpi_driver_create_file(const struct device_driver *driver,
			    const struct driver_attribute *attr);
void lkpi_driver_remove_file(const struct device_driver *driver,
			     const struct driver_attribute *attr);
void lkpi_wait_for_device_probe(void);

#define driver_register(d) lkpi_driver_register(d)
#define driver_unregister(d) lkpi_driver_unregister(d)
#define driver_find(n, b) lkpi_driver_find(n, b)
#define driver_for_each_device(d, s, x, f) lkpi_driver_for_each_device(d, s, x, f)
#define driver_find_device(d, s, x, m) lkpi_driver_find_device(d, s, x, m)
#define driver_create_file(d, a) lkpi_driver_create_file(d, a)
#define driver_remove_file(d, a) lkpi_driver_remove_file(d, a)
#define wait_for_device_probe() lkpi_wait_for_device_probe()
static inline struct device *driver_find_device_by_name(const struct device_driver *drv,
							const char *name)
{
	return driver_find_device(drv, NULL, name, device_match_name);
}

/* Un modulo que solo registra un driver (module_driver y variantes). */
#define module_driver(__driver, __register, __unregister, ...) \
static int __init __driver##_init(void) \
{ \
	return __register(&(__driver), ##__VA_ARGS__); \
} \
module_init(__driver##_init); \
static void __exit __driver##_exit(void) \
{ \
	__unregister(&(__driver), ##__VA_ARGS__); \
} \
module_exit(__driver##_exit);
#define builtin_driver(__driver, __register, ...) \
static int __init __driver##_init(void) \
{ \
	return __register(&(__driver), ##__VA_ARGS__); \
} \
module_init(__driver##_init);

/* --------------------------------------------------------------- class */

struct class_attribute {
	struct attribute attr;
	ssize_t (*show)(const struct class *class, const struct class_attribute *attr, char *buf);
	ssize_t (*store)(const struct class *class, const struct class_attribute *attr,
			 const char *buf, size_t count);
};
#define CLASS_ATTR_RW(_name) struct class_attribute class_attr_##_name = __ATTR_RW(_name)
#define CLASS_ATTR_RO(_name) struct class_attribute class_attr_##_name = __ATTR_RO(_name)
#define CLASS_ATTR_WO(_name) struct class_attribute class_attr_##_name = __ATTR_WO(_name)

struct class_attribute_string {
	struct class_attribute attr;
	char *str;
};
ssize_t lkpi_show_class_attr_string(const struct class *class, const struct class_attribute *attr,
				    char *buf);
#define CLASS_ATTR_STRING(_name, _mode, _str) \
	struct class_attribute_string class_attr_##_name = { \
		.attr = { .attr = { .name = __stringify(_name), .mode = _mode }, \
			  .show = lkpi_show_class_attr_string }, \
		.str = _str }

struct class_dev_iter;
int lkpi_class_register(const struct class *cls);
void lkpi_class_unregister(const struct class *cls);
struct class *lkpi_class_create(const char *name);
void lkpi_class_destroy(const struct class *cls);
bool lkpi_class_is_registered(const struct class *cls);
int lkpi_class_for_each_device(const struct class *cls, const struct device *start, void *data,
			       int (*fn)(struct device *dev, void *data));
struct device *lkpi_class_find_device(const struct class *cls, const struct device *start,
				      const void *data,
				      int (*match)(struct device *, const void *));
int lkpi_class_create_file_ns(const struct class *cls, const struct class_attribute *attr,
			      const void *ns);
void lkpi_class_remove_file_ns(const struct class *cls, const struct class_attribute *attr,
			       const void *ns);
#define class_register(c) lkpi_class_register(c)
#define class_unregister(c) lkpi_class_unregister(c)
#define class_create(n) lkpi_class_create(n)
#define class_destroy(c) lkpi_class_destroy(c)
#define class_is_registered(c) lkpi_class_is_registered(c)
#define class_for_each_device(c, s, d, f) lkpi_class_for_each_device(c, s, d, f)
#define class_find_device(c, s, d, m) lkpi_class_find_device(c, s, d, m)
#define class_create_file(c, a) lkpi_class_create_file_ns(c, a, NULL)
#define class_remove_file(c, a) lkpi_class_remove_file_ns(c, a, NULL)
static inline struct device *class_find_device_by_name(const struct class *cls, const char *name)
{
	return class_find_device(cls, NULL, name, device_match_name);
}
static inline struct device *class_find_device_by_devt(const struct class *cls, dev_t devt)
{
	return class_find_device(cls, NULL, &devt, device_match_devt);
}

/* -------------------------------------------------------------- devres */

typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);

void *lkpi_devres_alloc(dr_release_t release, size_t size, gfp_t gfp, const char *name);
void lkpi_devres_free(void *res);
void lkpi_devres_add(struct device *dev, void *res);
void *lkpi_devres_find(struct device *dev, dr_release_t release, dr_match_t match,
		       void *match_data);
void *lkpi_devres_get(struct device *dev, void *new_res, dr_match_t match, void *match_data);
void *lkpi_devres_remove(struct device *dev, dr_release_t release, dr_match_t match,
			 void *match_data);
int lkpi_devres_destroy(struct device *dev, dr_release_t release, dr_match_t match,
			void *match_data);
int lkpi_devres_release(struct device *dev, dr_release_t release, dr_match_t match,
			void *match_data);
void lkpi_devres_for_each_res(struct device *dev, dr_release_t release, dr_match_t match,
			      void *match_data,
			      void (*fn)(struct device *, void *, void *), void *data);
void *lkpi_devres_open_group(struct device *dev, void *id, gfp_t gfp);
void lkpi_devres_close_group(struct device *dev, void *id);
void lkpi_devres_remove_group(struct device *dev, void *id);
int lkpi_devres_release_group(struct device *dev, void *id);
int lkpi_devres_release_all(struct device *dev);

#define devres_alloc(release, size, gfp) lkpi_devres_alloc(release, size, gfp, #release)
#define devres_alloc_node(release, size, gfp, nid) devres_alloc(release, size, gfp)
#define devres_free(r) lkpi_devres_free(r)
#define devres_add(d, r) lkpi_devres_add(d, r)
#define devres_find(d, r, m, x) lkpi_devres_find(d, r, m, x)
#define devres_get(d, r, m, x) lkpi_devres_get(d, r, m, x)
#define devres_remove(d, r, m, x) lkpi_devres_remove(d, r, m, x)
#define devres_destroy(d, r, m, x) lkpi_devres_destroy(d, r, m, x)
#define devres_release(d, r, m, x) lkpi_devres_release(d, r, m, x)
#define devres_for_each_res(d, r, m, x, f, y) lkpi_devres_for_each_res(d, r, m, x, f, y)
#define devres_open_group(d, i, g) lkpi_devres_open_group(d, i, g)
#define devres_close_group(d, i) lkpi_devres_close_group(d, i)
#define devres_remove_group(d, i) lkpi_devres_remove_group(d, i)
#define devres_release_group(d, i) lkpi_devres_release_group(d, i)

void *lkpi_devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) __malloc;
void *lkpi_devm_krealloc(struct device *dev, void *ptr, size_t size, gfp_t gfp);
void lkpi_devm_kfree(struct device *dev, const void *p);
char *lkpi_devm_kstrdup(struct device *dev, const char *s, gfp_t gfp) __malloc;
const char *lkpi_devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp);
void *lkpi_devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp);
__printf(3, 0) char *lkpi_devm_kvasprintf(struct device *dev, gfp_t gfp, const char *fmt,
					  va_list ap);
__printf(3, 4) char *lkpi_devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...);
int lkpi_devm_add_action(struct device *dev, void (*action)(void *), void *data,
			 const char *name);
void lkpi_devm_remove_action(struct device *dev, void (*action)(void *), void *data);
void lkpi_devm_release_action(struct device *dev, void (*action)(void *), void *data);
unsigned long lkpi_devm_get_free_pages(struct device *dev, gfp_t gfp, unsigned int order);
void lkpi_devm_free_pages(struct device *dev, unsigned long addr);

#define devm_kmalloc(d, s, g) lkpi_devm_kmalloc(d, s, g)
#define devm_krealloc(d, p, s, g) lkpi_devm_krealloc(d, p, s, g)
#define devm_kfree(d, p) lkpi_devm_kfree(d, p)
#define devm_kstrdup(d, s, g) lkpi_devm_kstrdup(d, s, g)
#define devm_kstrdup_const(d, s, g) lkpi_devm_kstrdup_const(d, s, g)
#define devm_kmemdup(d, s, l, g) lkpi_devm_kmemdup(d, s, l, g)
#define devm_kvasprintf(d, g, f, a) lkpi_devm_kvasprintf(d, g, f, a)
#define devm_kasprintf lkpi_devm_kasprintf
#define devm_add_action(d, a, x) lkpi_devm_add_action(d, a, x, #a)
#define devm_remove_action(d, a, x) lkpi_devm_remove_action(d, a, x)
#define devm_release_action(d, a, x) lkpi_devm_release_action(d, a, x)
#define devm_get_free_pages(d, g, o) lkpi_devm_get_free_pages(d, g, o)
#define devm_free_pages(d, a) lkpi_devm_free_pages(d, a)

static inline void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
	return devm_kmalloc(dev, size, gfp | __GFP_ZERO);
}
static inline void *devm_kmalloc_array(struct device *dev, size_t n, size_t size, gfp_t flags)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	return devm_kmalloc(dev, bytes, flags);
}
static inline void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t flags)
{
	return devm_kmalloc_array(dev, n, size, flags | __GFP_ZERO);
}
static inline int __devm_add_action_or_reset(struct device *dev, void (*action)(void *),
					     void *data, const char *name)
{
	int ret = lkpi_devm_add_action(dev, action, data, name);
	if (ret)
		action(data);
	return ret;
}
#define devm_add_action_or_reset(d, a, x) __devm_add_action_or_reset(d, a, x, #a)

/* Regiones MMIO con devres (lib/devres.c en Linux). */
void __iomem *lkpi_devm_ioremap(struct device *dev, resource_size_t offset,
				resource_size_t size, int type);
void lkpi_devm_iounmap(struct device *dev, void __iomem *addr);
void __iomem *lkpi_devm_ioremap_resource(struct device *dev, const struct resource *res,
					 int type);
#define devm_ioremap(d, o, s) lkpi_devm_ioremap(d, o, s, 0)
void *lkpi_memremap(phys_addr_t offset, size_t size, unsigned long flags);
void lkpi_memunmap(void *addr);
static inline void lkpi_devm_memunmap(void *p) { lkpi_memunmap(p); }
static inline void *devm_memremap(struct device *dev, resource_size_t offset, size_t size,
				  unsigned long flags)
{
	void *p = lkpi_memremap(offset, size, flags);

	if (!p)
		return ERR_PTR(-ENXIO);
	if (devm_add_action_or_reset(dev, lkpi_devm_memunmap, p))
		return ERR_PTR(-ENOMEM);
	return p;
}
#define devm_ioremap_uc(d, o, s) lkpi_devm_ioremap(d, o, s, 0)
#define devm_ioremap_wc(d, o, s) lkpi_devm_ioremap(d, o, s, 1)
#define devm_iounmap(d, a) lkpi_devm_iounmap(d, a)
#define devm_ioremap_resource(d, r) lkpi_devm_ioremap_resource(d, r, 0)
#define devm_ioremap_resource_wc(d, r) lkpi_devm_ioremap_resource(d, r, 1)

/* ---------------------------------------------------------- dev_printk */

__printf(3, 4) void lkpi_dev_printk(const char *level, const struct device *dev,
				    const char *fmt, ...);
#define dev_printk(level, dev, fmt, ...) lkpi_dev_printk(level, dev, fmt, ##__VA_ARGS__)
#define dev_emerg(dev, fmt, ...) dev_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...) dev_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...) dev_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...) dev_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) dev_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) dev_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) dev_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#if defined(DEBUG)
#define dev_dbg(dev, fmt, ...) dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__)
#else
#define dev_dbg(dev, fmt, ...) \
	({ if (0) dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__); })
#endif
#define dev_vdbg dev_dbg
#define dev_level_once(dev_level, dev, fmt, ...) \
	do { \
		static bool __print_once; \
		if (!__print_once) { \
			__print_once = true; \
			dev_level(dev, fmt, ##__VA_ARGS__); \
		} \
	} while (0)
#define dev_emerg_once(dev, fmt, ...) dev_level_once(dev_emerg, dev, fmt, ##__VA_ARGS__)
#define dev_alert_once(dev, fmt, ...) dev_level_once(dev_alert, dev, fmt, ##__VA_ARGS__)
#define dev_crit_once(dev, fmt, ...) dev_level_once(dev_crit, dev, fmt, ##__VA_ARGS__)
#define dev_err_once(dev, fmt, ...) dev_level_once(dev_err, dev, fmt, ##__VA_ARGS__)
#define dev_warn_once(dev, fmt, ...) dev_level_once(dev_warn, dev, fmt, ##__VA_ARGS__)
#define dev_notice_once(dev, fmt, ...) dev_level_once(dev_notice, dev, fmt, ##__VA_ARGS__)
#define dev_info_once(dev, fmt, ...) dev_level_once(dev_info, dev, fmt, ##__VA_ARGS__)
#define dev_dbg_once(dev, fmt, ...) dev_level_once(dev_dbg, dev, fmt, ##__VA_ARGS__)
/* Sin limitacion de ritmo todavia (como pr_*_ratelimited). */
#define dev_err_ratelimited dev_err
#define dev_warn_ratelimited dev_warn
#define dev_notice_ratelimited dev_notice
#define dev_info_ratelimited dev_info
#define dev_dbg_ratelimited dev_dbg
#define dev_WARN(dev, format, arg...) WARN(1, "%s %s: " format, dev_driver_string(dev), \
					   dev_name(dev), ##arg)
#define dev_WARN_ONCE(dev, condition, format, arg...) \
	WARN_ONCE(condition, "%s %s: " format, dev_driver_string(dev), dev_name(dev), ##arg)
#define dev_warn_probe(dev, err, fmt, ...) dev_err_probe(dev, err, fmt, ##__VA_ARGS__)

#include <linux/ratelimit.h>


static inline void device_link_remove(void *consumer, struct device *supplier) {}
/* Sin IOMMU hasta L9 */
static inline bool device_iommu_mapped(struct device *dev) { return false; }
/* dev_printk_emit (syslog estructurado): aqui un dev_printk plano (N2). */
#define dev_printk_emit(lvl, dev, fmt, ...) ({ lkpi_dev_printk("", dev, fmt, ##__VA_ARGS__); (int)0; })
#endif
