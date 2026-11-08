#ifndef _LKPI_NET_FAILOVER_H
#define _LKPI_NET_FAILOVER_H
#include <linux/netdevice.h>
/* Sin failover de red (VF + virtio): la funcion BACKUP no se anuncia. */
struct failover;
static inline struct failover *net_failover_create(struct net_device *standby_dev) { return ERR_PTR(-EOPNOTSUPP); }
static inline void net_failover_destroy(struct failover *failover) { }
#endif
