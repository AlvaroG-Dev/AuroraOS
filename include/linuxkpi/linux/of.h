/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Device tree: en x86 no hay (Aurora tampoco lo ofrece). Como un kernel de
 * Linux sin CONFIG_OF: los tipos existen y las funciones dicen "no hay nodo"
 * (NULL, 0, false, -ENOSYS), asi los drivers toman su camino sin DT.
 */
#ifndef _LKPI_LINUX_OF_H
#define _LKPI_LINUX_OF_H

#include <linux/types.h>
#include <linux/errno.h>

struct property {
	char *name;
	int length;
	void *value;
	struct property *next;
};

struct device_node {
	const char *name;
	const char *full_name;
	struct property *properties;
	struct device_node *parent;
};

struct of_phandle_args {
	struct device_node *np;
	int args_count;
	u32 args[16];
};

static inline struct device_node *of_node_get(struct device_node *node) { return node; }
static inline void of_node_put(struct device_node *node) { }
static inline bool of_property_present(const struct device_node *np, const char *propname)
{
	return false;
}
static inline int of_property_read_u32(const struct device_node *np, const char *propname,
				       u32 *out_value)
{
	return -ENOSYS;
}
static inline int of_property_read_u64(const struct device_node *np, const char *propname,
				       u64 *out_value)
{
	return -ENOSYS;
}
static inline int of_property_read_string(const struct device_node *np, const char *propname,
					  const char **out_string)
{
	return -ENOSYS;
}
static inline bool of_property_read_bool(const struct device_node *np, const char *propname)
{
	return false;
}
static inline struct device_node *of_parse_phandle(const struct device_node *np,
						   const char *phandle_name, int index)
{
	return NULL;
}
static inline int of_count_phandle_with_args(const struct device_node *np,
					     const char *list_name, const char *cells_name)
{
	return -ENOENT;
}
static inline int of_device_is_compatible(const struct device_node *device, const char *compat)
{
	return 0;
}
static inline bool of_device_is_available(const struct device_node *device) { return false; }
static inline int of_get_available_child_count(const struct device_node *np) { return 0; }
static inline int of_get_child_count(const struct device_node *np) { return 0; }
static inline struct device_node *of_get_next_available_child(const struct device_node *node,
							      struct device_node *prev)
{
	return NULL;
}
static inline struct device_node *of_get_next_child(const struct device_node *node,
						    struct device_node *prev)
{
	return NULL;
}
static inline struct device_node *of_find_compatible_node(struct device_node *from,
							  const char *type, const char *compat)
{
	return NULL;
}
static inline bool is_of_node(const void *fwnode) { return false; }
#define for_each_property_of_node(dn, pp) \
	for (pp = (dn) ? (dn)->properties : NULL; pp != NULL; pp = pp->next)
#define for_each_available_child_of_node(parent, child) \
	for (child = of_get_next_available_child(parent, NULL); child != NULL; \
	     child = of_get_next_available_child(parent, child))
#define for_each_child_of_node(parent, child) \
	for (child = of_get_next_child(parent, NULL); child != NULL; \
	     child = of_get_next_child(parent, child))
#define of_match_ptr(_ptr) NULL
#define of_node_full_name(np) ((np) ? (np)->full_name : "<no-node>")

#endif
