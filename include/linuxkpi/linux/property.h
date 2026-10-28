/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Propiedades de firmware (device tree, ACPI, software nodes): la capa no
 * tiene ninguna fuente, asi que todo falla con -ENXIO / devuelve NULL.
 */
#ifndef _LKPI_LINUX_PROPERTY_H
#define _LKPI_LINUX_PROPERTY_H

#include <linux/types.h>
#include <linux/err.h>

struct device;
struct fwnode_handle {
	struct fwnode_handle *secondary;
	const void *ops;
};

static inline struct fwnode_handle *fwnode_handle_get(struct fwnode_handle *fwnode) { return fwnode; }
static inline void fwnode_handle_put(struct fwnode_handle *fwnode) { }
static inline bool is_software_node(const struct fwnode_handle *fwnode) { return false; }
static inline bool device_property_present(const struct device *dev, const char *propname) { return false; }
static inline bool device_property_read_bool(const struct device *dev, const char *propname) { return false; }
static inline int device_property_read_u32(const struct device *dev, const char *propname, u32 *val)
{
	return -ENXIO;
}
static inline int device_property_read_string(const struct device *dev, const char *propname, const char **val)
{
	return -ENXIO;
}
static inline bool fwnode_property_present(const struct fwnode_handle *fwnode, const char *propname)
{
	return false;
}
static inline int fwnode_property_read_u32(const struct fwnode_handle *fwnode, const char *propname, u32 *val)
{
	return -ENXIO;
}


static inline struct fwnode_handle *device_get_next_child_node(const struct device *dev, struct fwnode_handle *child)
{
	return NULL;
}
static inline bool fwnode_device_is_available(const struct fwnode_handle *fwnode) { return false; }
static inline struct fwnode_handle *fwnode_find_reference(const struct fwnode_handle *fwnode, const char *name,
							  unsigned int index)
{
	return ERR_PTR(-ENOENT);
}
#endif
