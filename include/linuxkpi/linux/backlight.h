/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Retroiluminacion (K6, L4): clase "backlight" con los atributos de Linux en
 * /sys/class/backlight/<nombre> (brightness, actual_brightness,
 * max_brightness, bl_power, type, scale). Sin notificaciones a fbdev.
 */
#ifndef _LKPI_LINUX_BACKLIGHT_H
#define _LKPI_LINUX_BACKLIGHT_H

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define BACKLIGHT_POWER_ON 0
#define BACKLIGHT_POWER_OFF 4
#define BACKLIGHT_POWER_REDUCED 1
#define FB_BLANK_UNBLANK BACKLIGHT_POWER_ON
#define FB_BLANK_POWERDOWN BACKLIGHT_POWER_OFF

enum backlight_update_reason { BACKLIGHT_UPDATE_HOTKEY, BACKLIGHT_UPDATE_SYSFS };
enum backlight_type { BACKLIGHT_RAW = 1, BACKLIGHT_PLATFORM, BACKLIGHT_FIRMWARE, BACKLIGHT_TYPE_MAX };
enum backlight_scale { BACKLIGHT_SCALE_UNKNOWN = 0, BACKLIGHT_SCALE_LINEAR,
		       BACKLIGHT_SCALE_NON_LINEAR };

#define BL_CORE_SUSPENDED (1 << 0)
#define BL_CORE_FBBLANK (1 << 1)
#define BL_CORE_DRIVER1 (1 << 31)
#define BL_CORE_SUSPENDRESUME (1 << 0)

struct backlight_device;
struct fb_info;

struct backlight_ops {
	unsigned int options;
	int (*update_status)(struct backlight_device *);
	int (*get_brightness)(struct backlight_device *);
	bool (*controls_device)(struct backlight_device *bd, struct device *display_dev);
};

struct backlight_properties {
	int brightness;
	int max_brightness;
	int power;
	enum backlight_type type;
	unsigned int state;
	enum backlight_scale scale;
};

struct backlight_device {
	struct backlight_properties props;
	struct mutex update_lock;
	struct mutex ops_lock;
	const struct backlight_ops *ops;
	struct device dev;
	bool fb_bl_on[32];
	int use_count;
	struct list_head entry;
};
#define to_backlight_device(obj) container_of(obj, struct backlight_device, dev)

struct backlight_device *lkpi_backlight_device_register(const char *name, struct device *dev,
						       void *devdata,
						       const struct backlight_ops *ops,
						       const struct backlight_properties *props);
struct backlight_device *lkpi_devm_backlight_device_register(struct device *dev,
							    const char *name,
							    struct device *parent,
							    void *devdata,
							    const struct backlight_ops *ops,
							    const struct backlight_properties *props);
void lkpi_backlight_device_unregister(struct backlight_device *bd);
int lkpi_backlight_device_set_brightness(struct backlight_device *bd, unsigned long brightness);
struct backlight_device *lkpi_backlight_device_get_by_name(const char *name);
void lkpi_backlight_force_update(struct backlight_device *bd, enum backlight_update_reason r);
#define backlight_device_register(n, d, dd, o, p) lkpi_backlight_device_register(n, d, dd, o, p)
#define devm_backlight_device_register(d, n, pa, dd, o, p) \
	lkpi_devm_backlight_device_register(d, n, pa, dd, o, p)
#define backlight_device_unregister(b) lkpi_backlight_device_unregister(b)
#define devm_backlight_device_unregister(d, b) lkpi_backlight_device_unregister(b)
#define backlight_device_set_brightness(b, v) lkpi_backlight_device_set_brightness(b, v)
#define backlight_device_get_by_name(n) lkpi_backlight_device_get_by_name(n)
#define backlight_force_update(b, r) lkpi_backlight_force_update(b, r)

static inline int backlight_update_status(struct backlight_device *bd)
{
	int ret = -ENOENT;

	mutex_lock(&bd->update_lock);
	if (bd->ops && bd->ops->update_status)
		ret = bd->ops->update_status(bd);
	mutex_unlock(&bd->update_lock);
	return ret;
}
static inline bool backlight_is_blank(const struct backlight_device *bd)
{
	return bd->props.power != BACKLIGHT_POWER_ON ||
	       bd->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK);
}
static inline int backlight_get_brightness(const struct backlight_device *bd)
{
	return backlight_is_blank(bd) ? 0 : bd->props.brightness;
}
static inline int backlight_enable(struct backlight_device *bd)
{
	if (!bd)
		return 0;
	bd->props.power = BACKLIGHT_POWER_ON;
	bd->props.state &= ~BL_CORE_FBBLANK;
	return backlight_update_status(bd);
}
static inline int backlight_disable(struct backlight_device *bd)
{
	if (!bd)
		return 0;
	bd->props.power = BACKLIGHT_POWER_OFF;
	bd->props.state |= BL_CORE_FBBLANK;
	return backlight_update_status(bd);
}
static inline void *bl_get_data(struct backlight_device *bl_dev)
{
	return dev_get_drvdata(&bl_dev->dev);
}
static inline void backlight_put(struct backlight_device *bd)
{
	if (bd)
		put_device(&bd->dev);
}

#endif
