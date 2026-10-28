/* SPDX-License-Identifier: GPL-2.0-only */
/* Reguladores: ninguno en la capa; los opcionales no existen. */
#ifndef _LKPI_LINUX_REGULATOR_CONSUMER_H
#define _LKPI_LINUX_REGULATOR_CONSUMER_H

#include <linux/err.h>

struct device;
struct regulator;

static inline struct regulator *regulator_get(struct device *dev, const char *id)
{
	return ERR_PTR(-ENODEV);
}
static inline struct regulator *regulator_get_optional(struct device *dev, const char *id)
{
	return ERR_PTR(-ENODEV);
}
static inline void regulator_put(struct regulator *r) { }
static inline int regulator_enable(struct regulator *r) { return 0; }
static inline int regulator_disable(struct regulator *r) { return 0; }

#endif
