/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_GPIO_CONSUMER_H
#define _LKPI_LINUX_GPIO_CONSUMER_H
#include <linux/err.h>
#include <linux/device.h>
struct gpio_desc;
enum gpiod_flags {
	GPIOD_ASIS = 0, GPIOD_IN = 1, GPIOD_OUT_LOW = 3, GPIOD_OUT_HIGH = 7,
};
static inline struct gpio_desc *devm_gpiod_get(struct device *dev, const char *con_id, enum gpiod_flags flags)
{
	return ERR_PTR(-ENOENT);
}
static inline struct gpio_desc *devm_gpiod_get_index(struct device *dev, const char *con_id, unsigned int idx,
						     enum gpiod_flags flags)
{
	return ERR_PTR(-ENOENT);
}
static inline void gpiod_set_value(struct gpio_desc *desc, int value) {}
static inline void gpiod_set_value_cansleep(struct gpio_desc *desc, int value) {}
static inline int gpiod_get_value(const struct gpio_desc *desc) { return 0; }
#endif
