/* SPDX-License-Identifier: GPL-2.0-only */
/* LinuxKPI (N2, NVIDIA kernel-open): un solo nodo NUMA. */
#ifndef _LKPI_NODEMASK_H
#define _LKPI_NODEMASK_H
#include <linux/types.h>
#include <linux/mmzone.h>
enum node_states { N_POSSIBLE, N_ONLINE, N_NORMAL_MEMORY, N_HIGH_MEMORY, N_MEMORY, N_CPU, NR_NODE_STATES };
static inline bool node_state(int node, enum node_states state) { return node == 0; }
static inline int num_possible_nodes(void) { return 1; }
static inline int num_online_nodes(void) { return 1; }
static inline bool node_online(int node) { return node == 0; }
#define for_each_online_node(n) for ((n) = 0; (n) < 1; (n)++)
#define for_each_node(n) for_each_online_node(n)
#define node_isset(n, mask) ((n) == 0)
#endif
