/* inetdevice: Aurora no expone in_device a los drivers; ARP offload/ifa son no-ops. */
#ifndef _LKPI_LINUX_INETDEVICE_H
#define _LKPI_LINUX_INETDEVICE_H
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
struct in_ifaddr { struct in_ifaddr *ifa_next; __be32 ifa_local; __be32 ifa_address; };
struct in_device { struct in_ifaddr *ifa_list; };
#define NETDEV_NOTIFY_IFA_UP 1
#define NETDEV_NOTIFY_IFA_DOWN 2
static inline struct in_device *__in_dev_get_rtnl(const struct net_device *dev) { return NULL; }
static inline int register_inetaddr_notifier(struct notifier_block *nb) { return 0; }
static inline int unregister_inetaddr_notifier(struct notifier_block *nb) { return 0; }
#define NETDEV_UP_IFA NETDEV_UP
#endif
