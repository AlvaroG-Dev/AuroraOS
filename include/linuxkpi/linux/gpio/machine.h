/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_GPIO_MACHINE_H
#define _LKPI_LINUX_GPIO_MACHINE_H
/* Sin subsistema GPIO: las tablas se aceptan y ningun gpiod_get() las resuelve. */
#include <linux/types.h>
#include <linux/list.h>
#include <linux/err.h>
enum gpio_lookup_flags {
	GPIO_ACTIVE_HIGH = (0 << 0), GPIO_ACTIVE_LOW = (1 << 0), GPIO_OPEN_DRAIN = (1 << 1),
	GPIO_OPEN_SOURCE = (1 << 2), GPIO_PERSISTENT = (0 << 3), GPIO_TRANSITORY = (1 << 3),
	GPIO_PULL_UP = (1 << 4), GPIO_PULL_DOWN = (1 << 5), GPIO_PULL_DISABLE = (1 << 6),
};
struct gpiod_lookup {
	const char *key;
	unsigned int chip_hwnum;
	const char *con_id;
	unsigned int idx;
	unsigned long flags;
};
struct gpiod_lookup_table {
	struct list_head list;
	const char *dev_id;
	struct gpiod_lookup table[];
};
#define GPIO_LOOKUP(_key, _chip_hwnum, _con_id, _flags) GPIO_LOOKUP_IDX(_key, _chip_hwnum, _con_id, 0, _flags)
#define GPIO_LOOKUP_IDX(_key, _chip_hwnum, _con_id, _idx, _flags) \
	(struct gpiod_lookup){ .key = _key, .chip_hwnum = _chip_hwnum, .con_id = _con_id, .idx = _idx, .flags = _flags }
static inline void gpiod_add_lookup_table(struct gpiod_lookup_table *table) {}
static inline void gpiod_remove_lookup_table(struct gpiod_lookup_table *table) {}
#endif
