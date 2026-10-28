/* SPDX-License-Identifier: GPL-2.0-only */
/* Grafo de puertos del device tree: sin DT no hay endpoints. */
#ifndef _LKPI_LINUX_OF_GRAPH_H
#define _LKPI_LINUX_OF_GRAPH_H

#include <linux/of.h>

struct of_endpoint {
	unsigned int port;
	unsigned int id;
	const struct fwnode_handle *local_fwnode;
};

static inline struct device_node *of_graph_get_remote_node(const struct device_node *node, u32 port, u32 endpoint)
{
	return NULL;
}
static inline struct device_node *of_graph_get_port_by_id(struct device_node *node, u32 id) { return NULL; }
static inline struct device_node *of_graph_get_remote_port_parent(const struct device_node *node) { return NULL; }
static inline struct device_node *of_graph_get_remote_endpoint(const struct device_node *node) { return NULL; }
static inline struct device_node *of_graph_get_endpoint_by_regs(const struct device_node *parent, int port_reg, int reg)
{
	return NULL;
}

#endif
