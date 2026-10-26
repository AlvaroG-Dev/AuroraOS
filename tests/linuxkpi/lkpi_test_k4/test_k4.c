// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de K4 (L3): kobject/kref, sysfs (atributos y grupos en el /sys
 * real), bus_type, device_driver, probe/remove, probe diferido, devres,
 * clases, notificadores del bus y bus de plataforma.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/kref.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "lkpi_test.h"

/* Tipo de `path` en /sys y, si es fichero o enlace, su contenido en out. */
static int kt_sys(const char *path, char *out, size_t outlen)
{
	char *data = NULL;
	size_t len = 0;
	int kind = kt_sysfs_get(path, &data, &len);

	if (out && outlen) {
		out[0] = '\0';
		if (data) {
			strscpy(out, data, outlen);
		}
	}
	if (data)
		kt_sysfs_free(data);
	return kind;
}

#define KT_SYS_IS(path, kind) \
	do { \
		int __k = kt_sys(path, NULL, 0); \
		KT_ASSERT(__k == (kind), "%s: tipo %d, esperado %d", path, __k, kind); \
	} while (0)

#define KT_SYS_CONTENT(path, kind, expected) \
	do { \
		char __b[128]; \
		int __k = kt_sys(path, __b, sizeof(__b)); \
		KT_ASSERT(__k == (kind) && !strcmp(__b, expected), "%s: tipo %d '%s', esperado '%s'", \
			  path, __k, __b, expected); \
	} while (0)

/* ----------------------------------------------------------- kobject */

static int kt_val = 7;
static int kt_kobj_released;

static ssize_t valor_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", kt_val);
}

static ssize_t valor_store(struct kobject *k, struct kobj_attribute *a, const char *buf,
			   size_t count)
{
	int rc = kstrtoint(buf, 10, &kt_val);

	return rc ? rc : count;
}

static struct kobj_attribute kt_attr_valor = __ATTR_RW(valor);

static ssize_t oculto_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "no deberia verse\n");
}
static ssize_t visible_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "si\n");
}
static struct kobj_attribute kt_attr_oculto = __ATTR_RO(oculto);
static struct kobj_attribute kt_attr_visible = __ATTR_RO(visible);
static struct attribute *kt_grp_attrs[] = { &kt_attr_oculto.attr, &kt_attr_visible.attr, NULL };

static umode_t kt_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	return attr == &kt_attr_oculto.attr ? 0 : attr->mode;
}

static const struct attribute_group kt_grp = {
	.name = "grupo",
	.attrs = kt_grp_attrs,
	.is_visible = kt_is_visible,
};

static void kt_kobj_release(struct kobject *kobj)
{
	kt_kobj_released++;
	kfree(kobj);
}

static const struct kobj_type kt_ktype = {
	.release = kt_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static void test_kobject(void)
{
	struct kobject *k, *child;
	char *path;

	k = kzalloc(sizeof(*k), GFP_KERNEL);
	KT_ASSERT(k && kobject_init_and_add(k, &kt_ktype, kernel_kobj, "lkpi_k4") == 0,
		  "kobject_init_and_add");
	if (!k)
		return;
	KT_STREQ(kobject_name(k), "lkpi_k4");
	path = kobject_get_path(k, GFP_KERNEL);
	KT_STREQ(path, "/kernel/lkpi_k4");
	kfree(path);

	KT_EQ(sysfs_create_file(k, &kt_attr_valor.attr), 0);
	KT_SYS_CONTENT("kernel/lkpi_k4/valor", KT_SYSFS_ATTR, "7\n");
	KT_EQ(kt_sysfs_store("kernel/lkpi_k4/valor", "42", 2), 2);
	KT_EQ(kt_val, 42);
	KT_SYS_CONTENT("kernel/lkpi_k4/valor", KT_SYSFS_ATTR, "42\n");
	KT_EQ(sysfs_create_file(k, &kt_attr_valor.attr), -EEXIST);

	KT_EQ(sysfs_create_group(k, &kt_grp), 0);
	KT_SYS_CONTENT("kernel/lkpi_k4/grupo/visible", KT_SYSFS_ATTR, "si\n");
	KT_SYS_IS("kernel/lkpi_k4/grupo/oculto", KT_SYSFS_NONE);
	KT_SYS_IS("kernel/lkpi_k4/grupo", KT_SYSFS_DIR);
	sysfs_remove_group(k, &kt_grp);
	KT_SYS_IS("kernel/lkpi_k4/grupo/visible", KT_SYSFS_NONE);

	/* Hijo creado con kobject_create_and_add y enlace hacia el padre. */
	child = kobject_create_and_add("hijo", k);
	KT_ASSERT(child != NULL, "kobject_create_and_add");
	KT_EQ(sysfs_create_link(child, k, "padre"), 0);
	{
		char b[64];

		KT_EQ(kt_sys("kernel/lkpi_k4/hijo/padre", b, sizeof(b)), KT_SYSFS_LINK);
		KT_STREQ(b, "/sys/kernel/lkpi_k4");
	}
	kobject_put(child);
	KT_SYS_IS("kernel/lkpi_k4/hijo/padre", KT_SYSFS_NONE);

	kt_kobj_released = 0;
	kobject_get(k);
	kobject_put(k);
	KT_EQ(kt_kobj_released, 0);
	kobject_put(k);		/* ultima referencia: kobject_del + release */
	KT_EQ(kt_kobj_released, 1);
	KT_SYS_IS("kernel/lkpi_k4/valor", KT_SYSFS_NONE);
	KT_SYS_IS("kernel/lkpi_k4", KT_SYSFS_NONE);
}

struct kt_kref_obj {
	struct kref ref;
	int *released;
};

static void kt_kref_release(struct kref *ref)
{
	struct kt_kref_obj *o = container_of(ref, struct kt_kref_obj, ref);

	(*o->released)++;
	kfree(o);
}

static void test_kref(void)
{
	int released = 0;
	struct kt_kref_obj *o = kzalloc(sizeof(*o), GFP_KERNEL);

	if (!o) {
		KT_ASSERT(0, "sin memoria");
		return;
	}
	o->released = &released;
	kref_init(&o->ref);
	kref_get(&o->ref);
	KT_EQ(kref_read(&o->ref), 2);
	KT_EQ(kref_put(&o->ref, kt_kref_release), 0);
	KT_EQ(released, 0);
	KT_EQ(kref_put(&o->ref, kt_kref_release), 1);
	KT_EQ(released, 1);
}

/* ------------------------------------------------- bus, driver, device */

struct kt_dev {
	struct device dev;
	int probes;
};
#define to_kt_dev(d) container_of(d, struct kt_dev, dev)

static int kt_released, kt_probe_calls, kt_remove_calls, kt_devres_runs;
static bool kt_defer_ok;
static int kt_notify[16];

static int kt_match(struct device *dev, const struct device_driver *drv)
{
	return !strncmp(dev_name(dev), "kt-", 3);
}

static int kt_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	return add_uevent_var(env, "KT_BUS=1");
}

static const struct bus_type kt_bus = {
	.name = "lkpi_k4bus",
	.match = kt_match,
	.uevent = kt_uevent,
};

static void kt_devres_action(void *data)
{
	kt_devres_runs++;
}

static int kt_probe(struct device *dev)
{
	struct kt_dev *kd = to_kt_dev(dev);
	int *mem;

	kt_probe_calls++;
	if (!strcmp(dev_name(dev), "kt-defer") && !kt_defer_ok)
		return -EPROBE_DEFER;
	mem = devm_kzalloc(dev, 64, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;
	if (devm_add_action(dev, kt_devres_action, NULL))
		return -ENOMEM;
	kd->probes++;
	dev_set_drvdata(dev, mem);
	return 0;
}

static int kt_remove(struct device *dev)
{
	kt_remove_calls++;
	return 0;
}

static ssize_t estado_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "probes=%d\n", to_kt_dev(dev)->probes);
}
static DEVICE_ATTR_RO(estado);
static struct attribute *kt_drv_dev_attrs[] = { &dev_attr_estado.attr, NULL };
ATTRIBUTE_GROUPS(kt_drv_dev);

static struct device_driver kt_drv = {
	.name = "kt_drv",
	.bus = &kt_bus,
	.probe = kt_probe,
	.remove = kt_remove,
	.dev_groups = kt_drv_dev_groups,
};

static int kt_nb_call(struct notifier_block *nb, unsigned long action, void *data)
{
	if (action < ARRAY_SIZE(kt_notify))
		kt_notify[action]++;
	return NOTIFY_OK;
}
static struct notifier_block kt_nb = { .notifier_call = kt_nb_call };

static void kt_dev_release(struct device *dev)
{
	kt_released++;
	kfree(to_kt_dev(dev));
}

static struct kt_dev *kt_dev_new(const char *name)
{
	struct kt_dev *kd = kzalloc(sizeof(*kd), GFP_KERNEL);

	if (!kd)
		return NULL;
	device_initialize(&kd->dev);
	kd->dev.bus = &kt_bus;
	kd->dev.release = kt_dev_release;
	dev_set_name(&kd->dev, "%s", name);
	if (device_add(&kd->dev)) {
		put_device(&kd->dev);
		return NULL;
	}
	return kd;
}

static int kt_count_fn(struct device *dev, void *data)
{
	(*(int *)data)++;
	return 0;
}

static void test_bus_driver(void)
{
	struct kt_dev *a, *b, *c, *def;
	struct device *found;
	char buf[256];
	int n;

	kt_released = kt_probe_calls = kt_remove_calls = kt_devres_runs = 0;
	kt_defer_ok = false;
	memset(kt_notify, 0, sizeof(kt_notify));

	KT_EQ(bus_register(&kt_bus), 0);
	KT_EQ(bus_register_notifier(&kt_bus, &kt_nb), 0);
	KT_SYS_IS("bus/lkpi_k4bus", KT_SYSFS_DIR);

	a = kt_dev_new("kt-a");
	KT_ASSERT(a && !a->dev.driver, "kt-a sin driver todavia");
	KT_SYS_CONTENT("bus/lkpi_k4bus/devices/kt-a", KT_SYSFS_LINK, "/sys/devices/kt-a");
	KT_SYS_CONTENT("devices/kt-a/subsystem", KT_SYSFS_LINK, "/sys/bus/lkpi_k4bus");

	KT_EQ(driver_register(&kt_drv), 0);
	KT_ASSERT(a && a->dev.driver == &kt_drv, "driver_register vincula kt-a");
	KT_EQ(driver_register(&kt_drv), -EBUSY);
	b = kt_dev_new("kt-b");
	KT_ASSERT(b && b->dev.driver == &kt_drv, "device_add vincula kt-b");

	KT_SYS_CONTENT("bus/lkpi_k4bus/drivers/kt_drv/kt-a", KT_SYSFS_LINK, "/sys/devices/kt-a");
	KT_SYS_CONTENT("devices/kt-a/driver", KT_SYSFS_LINK, "/sys/bus/lkpi_k4bus/drivers/kt_drv");
	KT_SYS_CONTENT("devices/kt-a/estado", KT_SYSFS_ATTR, "probes=1\n");
	KT_EQ(kt_sys("devices/kt-a/uevent", buf, sizeof(buf)), KT_SYSFS_ATTR);
	KT_ASSERT(strstr(buf, "DRIVER=kt_drv\n") && strstr(buf, "KT_BUS=1\n"), "uevent: '%s'", buf);

	/* Probe diferido: se reintenta cuando otro dispositivo se vincula. */
	def = kt_dev_new("kt-defer");
	KT_ASSERT(def && !def->dev.driver, "kt-defer diferido");
	kt_defer_ok = true;
	c = kt_dev_new("kt-c");
	KT_ASSERT(c && c->dev.driver == &kt_drv, "kt-c vinculado");
	KT_ASSERT(def && def->dev.driver == &kt_drv, "kt-defer vinculado al reintentar");

	n = 0;
	KT_EQ(bus_for_each_dev(&kt_bus, NULL, &n, kt_count_fn), 0);
	KT_EQ(n, 4);
	found = bus_find_device_by_name(&kt_bus, NULL, "kt-c");
	KT_ASSERT(found == &c->dev, "bus_find_device_by_name");
	put_device(found);
	KT_EQ(kt_notify[BUS_NOTIFY_ADD_DEVICE], 4);
	KT_EQ(kt_notify[BUS_NOTIFY_BOUND_DRIVER], 4);
	KT_EQ(kt_notify[BUS_NOTIFY_DRIVER_NOT_BOUND], 1);

	/* Baja de un dispositivo vinculado: remove, devres y release. */
	device_unregister(&b->dev);
	KT_EQ(kt_remove_calls, 1);
	KT_EQ(kt_devres_runs, 1);
	KT_EQ(kt_released, 1);
	KT_SYS_IS("bus/lkpi_k4bus/drivers/kt_drv/kt-b", KT_SYSFS_NONE);
	KT_SYS_IS("devices/kt-b", KT_SYSFS_NONE);

	/* unbind/bind desde sysfs, como lo haria el userland. */
	KT_EQ(kt_sysfs_store("bus/lkpi_k4bus/drivers/kt_drv/unbind", "kt-a\n", 5), 5);
	KT_ASSERT(a && !a->dev.driver, "unbind por sysfs");
	KT_EQ(kt_remove_calls, 2);
	KT_SYS_IS("devices/kt-a/driver", KT_SYSFS_NONE);
	KT_EQ(kt_sysfs_store("bus/lkpi_k4bus/drivers/kt_drv/bind", "kt-a\n", 5), 5);
	KT_ASSERT(a && a->dev.driver == &kt_drv, "bind por sysfs");
	KT_SYS_CONTENT("devices/kt-a/estado", KT_SYSFS_ATTR, "probes=2\n");

	/* driver_unregister desvincula todo lo que queda. */
	driver_unregister(&kt_drv);
	KT_ASSERT(a && c && def && !a->dev.driver && !c->dev.driver && !def->dev.driver,
		  "driver_unregister desvincula");
	KT_EQ(kt_remove_calls, 5);
	KT_EQ(kt_devres_runs, 5);
	KT_SYS_IS("bus/lkpi_k4bus/drivers/kt_drv", KT_SYSFS_NONE);

	if (a)
		device_unregister(&a->dev);
	if (c)
		device_unregister(&c->dev);
	if (def)
		device_unregister(&def->dev);
	KT_EQ(kt_released, 4);
	KT_EQ(bus_unregister_notifier(&kt_bus, &kt_nb), 0);
	bus_unregister(&kt_bus);
	KT_SYS_IS("bus/lkpi_k4bus", KT_SYSFS_NONE);
}

/* ------------------------------------------------- devres (grupos) */

static void kt_plain_release(struct device *dev)
{
	kt_released++;
	kfree(dev);
}

static void test_devres(void)
{
	struct device *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	void *g1, *g2;
	char *s;

	if (!dev) {
		KT_ASSERT(0, "sin memoria");
		return;
	}
	kt_released = kt_devres_runs = 0;
	device_initialize(dev);
	dev->release = kt_plain_release;
	dev_set_name(dev, "lkpi-k4-devres");
	KT_EQ(device_add(dev), 0);

	g1 = devres_open_group(dev, NULL, GFP_KERNEL);
	KT_ASSERT(g1 != NULL, "devres_open_group");
	devm_add_action(dev, kt_devres_action, NULL);
	devm_add_action(dev, kt_devres_action, NULL);
	g2 = devres_open_group(dev, NULL, GFP_KERNEL);
	devm_add_action(dev, kt_devres_action, NULL);
	devres_close_group(dev, g2);
	devres_close_group(dev, g1);
	KT_EQ(devres_release_group(dev, g2), 1);
	KT_EQ(kt_devres_runs, 1);
	KT_EQ(devres_release_group(dev, g1), 2);
	KT_EQ(kt_devres_runs, 3);

	s = devm_kasprintf(dev, GFP_KERNEL, "x%d", 12);
	KT_STREQ(s, "x12");
	devm_kfree(dev, s);
	KT_ASSERT(devm_kstrdup(dev, "abc", GFP_KERNEL) != NULL, "devm_kstrdup");
	devm_add_action(dev, kt_devres_action, NULL);
	devm_release_action(dev, kt_devres_action, NULL);
	KT_EQ(kt_devres_runs, 4);
	KT_EQ(__devm_add_action_or_reset(dev, kt_devres_action, NULL, "x"), 0);
	device_unregister(dev);	/* devres que queda: al liberar el device */
	KT_EQ(kt_devres_runs, 5);
	KT_EQ(kt_released, 1);
}

/* ------------------------------------------------------------- clases */

static void test_class(void)
{
	const struct class *cls = class_create("lkpi_k4cls");
	struct device *d, *f;
	char buf[64];

	KT_ASSERT(!IS_ERR(cls), "class_create");
	if (IS_ERR(cls))
		return;
	KT_ASSERT(class_is_registered(cls), "class_is_registered");
	d = device_create(cls, NULL, MKDEV(240, 1), (void *)0x1234, "ktc%d", 1);
	KT_ASSERT(!IS_ERR(d), "device_create");
	if (IS_ERR(d)) {
		class_destroy(cls);
		return;
	}
	KT_ASSERT(dev_get_drvdata(d) == (void *)0x1234, "drvdata");
	KT_SYS_CONTENT("class/lkpi_k4cls/ktc1", KT_SYSFS_LINK, "/sys/devices/virtual/lkpi_k4cls/ktc1");
	KT_SYS_CONTENT("devices/virtual/lkpi_k4cls/ktc1/dev", KT_SYSFS_ATTR, "240:1\n");
	KT_EQ(kt_sys("devices/virtual/lkpi_k4cls/ktc1/uevent", buf, sizeof(buf)), KT_SYSFS_ATTR);
	KT_ASSERT(strstr(buf, "MAJOR=240\nMINOR=1\nDEVNAME=ktc1\n") != NULL, "uevent: '%s'", buf);
	f = class_find_device_by_devt(cls, MKDEV(240, 1));
	KT_ASSERT(f == d, "class_find_device_by_devt");
	put_device(f);
	device_destroy(cls, MKDEV(240, 1));
	KT_SYS_IS("class/lkpi_k4cls/ktc1", KT_SYSFS_NONE);
	KT_SYS_IS("devices/virtual/lkpi_k4cls/ktc1", KT_SYSFS_NONE);
	class_destroy(cls);
}

/* ------------------------------------------------------- plataforma */

static int kt_plat_probes, kt_plat_removes;
static bool kt_plat_region_ok, kt_plat_double_busy;

static int kt_plat_probe(struct platform_device *pdev)
{
	struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	kt_plat_probes++;
	if (!r)
		return -ENODEV;
	kt_plat_region_ok = devm_request_mem_region(&pdev->dev, r->start, resource_size(r),
						    "lkpi-k4") != NULL;
	kt_plat_double_busy = request_mem_region(r->start, 16, "otro") == NULL;
	return platform_get_irq_optional(pdev, 0) == -ENXIO ? 0 : -EINVAL;
}

static void kt_plat_remove(struct platform_device *pdev)
{
	kt_plat_removes++;
}

static struct platform_driver kt_plat_drv = {
	.probe = kt_plat_probe,
	.remove = kt_plat_remove,
	.driver = { .name = "lkpi-k4plat" },
};

static void test_platform(void)
{
	/* Una region que nadie usa: no se mapea, solo se reserva. */
	struct resource res = DEFINE_RES_MEM(0xfeb00000, 0x1000);
	struct platform_device *pdev;
	struct resource *again;

	kt_plat_probes = kt_plat_removes = 0;
	KT_EQ(platform_driver_register(&kt_plat_drv), 0);
	pdev = platform_device_register_simple("lkpi-k4plat", 3, &res, 1);
	KT_ASSERT(!IS_ERR(pdev), "platform_device_register_simple");
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&kt_plat_drv);
		return;
	}
	KT_STREQ(dev_name(&pdev->dev), "lkpi-k4plat.3");
	KT_EQ(kt_plat_probes, 1);
	KT_ASSERT(pdev->dev.driver == &kt_plat_drv.driver, "vinculado");
	KT_ASSERT(kt_plat_region_ok, "devm_request_mem_region");
	KT_ASSERT(kt_plat_double_busy, "la region ya estaba ocupada");
	KT_SYS_CONTENT("devices/platform/lkpi-k4plat.3/driver", KT_SYSFS_LINK,
		       "/sys/bus/platform/drivers/lkpi-k4plat");
	KT_SYS_CONTENT("bus/platform/devices/lkpi-k4plat.3", KT_SYSFS_LINK,
		       "/sys/devices/platform/lkpi-k4plat.3");
	platform_device_unregister(pdev);
	KT_EQ(kt_plat_removes, 1);
	/* devres libero la region al desvincular. */
	again = request_mem_region(0xfeb00000, 0x1000, "otra vez");
	KT_ASSERT(again != NULL, "region liberada por devres");
	if (again)
		release_mem_region(0xfeb00000, 0x1000);
	platform_driver_unregister(&kt_plat_drv);
	KT_SYS_IS("bus/platform/drivers/lkpi-k4plat", KT_SYSFS_NONE);
}

static int __init test_k4_init(void)
{
	test_kobject();
	test_kref();
	test_bus_driver();
	test_devres();
	test_class();
	test_platform();
	return 0;
}

static void __exit test_k4_exit(void)
{
}

module_init(test_k4_init);
module_exit(test_k4_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pruebas LinuxKPI K4");
