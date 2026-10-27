// SPDX-License-Identifier: GPL-2.0-only
/* Clase backlight (K6, L4). Ver linux/backlight.h. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/kstrtox.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../lkpi_internal.h"

static LIST_HEAD(lkpi_backlights);
static DEFINE_MUTEX(lkpi_backlight_lock);

static const char *const lkpi_bl_types[] = {
	[BACKLIGHT_RAW] = "raw",
	[BACKLIGHT_PLATFORM] = "platform",
	[BACKLIGHT_FIRMWARE] = "firmware",
};
static const char *const lkpi_bl_scales[] = {
	[BACKLIGHT_SCALE_UNKNOWN] = "unknown",
	[BACKLIGHT_SCALE_LINEAR] = "linear",
	[BACKLIGHT_SCALE_NON_LINEAR] = "non-linear",
};

static ssize_t bl_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", to_backlight_device(dev)->props.power);
}

static ssize_t bl_power_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct backlight_device *bd = to_backlight_device(dev);
	unsigned long power;
	int rc = kstrtoul(buf, 0, &power);

	if (rc)
		return rc;
	if (power != BACKLIGHT_POWER_ON && power != BACKLIGHT_POWER_OFF &&
	    power != BACKLIGHT_POWER_REDUCED)
		return -EINVAL;
	rc = -ENXIO;
	mutex_lock(&bd->ops_lock);
	if (bd->ops) {
		bd->props.power = power;
		if (power == BACKLIGHT_POWER_ON)
			bd->props.state &= ~BL_CORE_FBBLANK;
		else
			bd->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(bd);
		rc = count;
	}
	mutex_unlock(&bd->ops_lock);
	return rc;
}
static DEVICE_ATTR_RW(bl_power);

static ssize_t brightness_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", to_backlight_device(dev)->props.brightness);
}

int lkpi_backlight_device_set_brightness(struct backlight_device *bd, unsigned long brightness)
{
	int rc = -ENXIO;

	mutex_lock(&bd->ops_lock);
	if (bd->ops) {
		if (brightness > (unsigned long)bd->props.max_brightness) {
			rc = -EINVAL;
		} else {
			bd->props.brightness = brightness;
			rc = backlight_update_status(bd);
		}
	}
	mutex_unlock(&bd->ops_lock);
	return rc;
}
EXPORT_SYMBOL(lkpi_backlight_device_set_brightness);

static ssize_t brightness_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long brightness;
	int rc = kstrtoul(buf, 0, &brightness);

	if (rc)
		return rc;
	rc = lkpi_backlight_device_set_brightness(to_backlight_device(dev), brightness);
	return rc ? rc : count;
}
static DEVICE_ATTR_RW(brightness);

static ssize_t type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	enum backlight_type t = to_backlight_device(dev)->props.type;

	return sysfs_emit(buf, "%s\n", t > 0 && t < BACKLIGHT_TYPE_MAX ? lkpi_bl_types[t] : "raw");
}
static DEVICE_ATTR_RO(type);

static ssize_t max_brightness_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", to_backlight_device(dev)->props.max_brightness);
}
static DEVICE_ATTR_RO(max_brightness);

static ssize_t actual_brightness_show(struct device *dev, struct device_attribute *attr,
				      char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	int rc = -ENXIO;

	mutex_lock(&bd->ops_lock);
	if (bd->ops && bd->ops->get_brightness)
		rc = bd->ops->get_brightness(bd);
	else if (bd->ops)
		rc = bd->props.brightness;
	mutex_unlock(&bd->ops_lock);
	return rc < 0 ? rc : sysfs_emit(buf, "%d\n", rc);
}
static DEVICE_ATTR_RO(actual_brightness);

static ssize_t scale_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	enum backlight_scale s = to_backlight_device(dev)->props.scale;

	return sysfs_emit(buf, "%s\n", s <= BACKLIGHT_SCALE_NON_LINEAR ? lkpi_bl_scales[s] :
			  "unknown");
}
static DEVICE_ATTR_RO(scale);

static struct attribute *lkpi_bl_attrs[] = {
	&dev_attr_bl_power.attr,
	&dev_attr_brightness.attr,
	&dev_attr_actual_brightness.attr,
	&dev_attr_max_brightness.attr,
	&dev_attr_scale.attr,
	&dev_attr_type.attr,
	NULL,
};
ATTRIBUTE_GROUPS(lkpi_bl);

static void lkpi_bl_release(struct device *dev)
{
	kfree(to_backlight_device(dev));
}

static const struct class lkpi_backlight_class = {
	.name = "backlight",
	.dev_groups = lkpi_bl_groups,
	.dev_release = lkpi_bl_release,
};

struct backlight_device *lkpi_backlight_device_register(const char *name, struct device *parent,
						       void *devdata,
						       const struct backlight_ops *ops,
						       const struct backlight_properties *props)
{
	struct backlight_device *bd;
	int rc;

	bd = kzalloc(sizeof(*bd), GFP_KERNEL);
	if (!bd)
		return ERR_PTR(-ENOMEM);
	mutex_init(&bd->update_lock);
	mutex_init(&bd->ops_lock);
	bd->dev.class = &lkpi_backlight_class;
	bd->dev.parent = parent;
	bd->dev.release = lkpi_bl_release;
	dev_set_name(&bd->dev, "%s", name);
	dev_set_drvdata(&bd->dev, devdata);
	if (props) {
		bd->props = *props;
		if (props->type <= 0 || props->type >= BACKLIGHT_TYPE_MAX)
			bd->props.type = BACKLIGHT_RAW;
	} else {
		bd->props.type = BACKLIGHT_RAW;
	}
	rc = device_register(&bd->dev);
	if (rc) {
		put_device(&bd->dev);
		return ERR_PTR(rc);
	}
	bd->ops = ops;
	mutex_lock(&lkpi_backlight_lock);
	list_add(&bd->entry, &lkpi_backlights);
	mutex_unlock(&lkpi_backlight_lock);
	return bd;
}
EXPORT_SYMBOL(lkpi_backlight_device_register);

void lkpi_backlight_device_unregister(struct backlight_device *bd)
{
	if (IS_ERR_OR_NULL(bd))
		return;
	mutex_lock(&lkpi_backlight_lock);
	list_del(&bd->entry);
	mutex_unlock(&lkpi_backlight_lock);
	mutex_lock(&bd->ops_lock);
	bd->ops = NULL;
	mutex_unlock(&bd->ops_lock);
	device_unregister(&bd->dev);
}
EXPORT_SYMBOL(lkpi_backlight_device_unregister);

static void lkpi_devm_bl_release(void *bd)
{
	lkpi_backlight_device_unregister(bd);
}

struct backlight_device *lkpi_devm_backlight_device_register(struct device *dev,
							    const char *name,
							    struct device *parent,
							    void *devdata,
							    const struct backlight_ops *ops,
							    const struct backlight_properties *props)
{
	struct backlight_device *bd = lkpi_backlight_device_register(name, parent, devdata, ops,
								     props);
	int rc;

	if (IS_ERR(bd))
		return bd;
	rc = devm_add_action_or_reset(dev, lkpi_devm_bl_release, bd);
	return rc ? ERR_PTR(rc) : bd;
}
EXPORT_SYMBOL(lkpi_devm_backlight_device_register);

struct backlight_device *lkpi_backlight_device_get_by_name(const char *name)
{
	struct backlight_device *bd, *found = NULL;

	mutex_lock(&lkpi_backlight_lock);
	list_for_each_entry(bd, &lkpi_backlights, entry)
		if (!strcmp(dev_name(&bd->dev), name)) {
			found = bd;
			get_device(&bd->dev);
			break;
		}
	mutex_unlock(&lkpi_backlight_lock);
	return found;
}
EXPORT_SYMBOL(lkpi_backlight_device_get_by_name);

void lkpi_backlight_force_update(struct backlight_device *bd, enum backlight_update_reason r)
{
	mutex_lock(&bd->ops_lock);
	if (bd->ops && bd->ops->get_brightness) {
		int b = bd->ops->get_brightness(bd);

		if (b >= 0)
			bd->props.brightness = b;
	}
	mutex_unlock(&bd->ops_lock);
}
EXPORT_SYMBOL(lkpi_backlight_force_update);

int lkpi_backlight_init(void)
{
	return class_register(&lkpi_backlight_class);
}

void lkpi_backlight_exit(void)
{
	class_unregister(&lkpi_backlight_class);
}
