/* SPDX-License-Identifier: GPL-2.0-only */
/* Reloj comun: la capa no tiene arbol de relojes; todos los relojes opcionales son NULL. */
#ifndef _LKPI_LINUX_CLK_H
#define _LKPI_LINUX_CLK_H

#include <linux/err.h>
#include <linux/types.h>

struct device;
struct device_node;
struct clk;

static inline struct clk *clk_get(struct device *dev, const char *id) { return ERR_PTR(-ENOENT); }
static inline struct clk *devm_clk_get(struct device *dev, const char *id) { return ERR_PTR(-ENOENT); }
static inline struct clk *devm_clk_get_optional(struct device *dev, const char *id) { return NULL; }
static inline void clk_put(struct clk *clk) { }
static inline int clk_prepare_enable(struct clk *clk) { return 0; }
static inline void clk_disable_unprepare(struct clk *clk) { }
static inline int clk_enable(struct clk *clk) { return 0; }
static inline void clk_disable(struct clk *clk) { }
static inline unsigned long clk_get_rate(struct clk *clk) { return 0; }

#endif
