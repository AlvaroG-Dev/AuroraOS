#ifndef _LKPI_NET_ADDRCONF_H
#define _LKPI_NET_ADDRCONF_H
#include <linux/netdevice.h>
#include <linux/in6.h>
/* IPv6 no esta disponible para los drivers: sin direcciones configuradas. */
static inline int ipv6_dev_get_saddr(struct net *net, const struct net_device *dev, const struct in6_addr *daddr, unsigned int prefs, struct in6_addr *saddr) { return -EADDRNOTAVAIL; }
#endif
