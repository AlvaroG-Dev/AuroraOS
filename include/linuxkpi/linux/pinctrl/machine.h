/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_PINCTRL_MACHINE_H
#define _LKPI_LINUX_PINCTRL_MACHINE_H
#include <linux/types.h>
enum pinctrl_map_type { PIN_MAP_TYPE_INVALID, PIN_MAP_TYPE_DUMMY_STATE, PIN_MAP_TYPE_MUX_GROUP,
			PIN_MAP_TYPE_CONFIGS_PIN, PIN_MAP_TYPE_CONFIGS_GROUP };
struct pinctrl_map_mux { const char *group; const char *function; };
struct pinctrl_map {
	const char *dev_name;
	const char *name;
	enum pinctrl_map_type type;
	const char *ctrl_dev_name;
	union { struct pinctrl_map_mux mux; } data;
};
#define PIN_MAP_MUX_GROUP(dev, state, pinctrl, grp, func) \
	{ .dev_name = dev, .name = state, .type = PIN_MAP_TYPE_MUX_GROUP, .ctrl_dev_name = pinctrl, \
	  .data.mux = { .group = grp, .function = func } }
static inline int pinctrl_register_mappings(const struct pinctrl_map *map, unsigned int num_maps) { return 0; }
#endif
