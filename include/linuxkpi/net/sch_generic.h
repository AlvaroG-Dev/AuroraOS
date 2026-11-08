/* Planificador de paquetes: no hay qdisc; las colas de TX estan siempre vacias. */
#ifndef _LKPI_NET_SCH_GENERIC_H
#define _LKPI_NET_SCH_GENERIC_H
#include <linux/netdevice.h>
static inline bool qdisc_all_tx_empty(const struct net_device *dev) { return true; }
#endif
