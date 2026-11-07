#ifndef _LKPI_NET_GSO_H
#define _LKPI_NET_GSO_H
#include <linux/skbuff.h>
#include <linux/netdevice.h>
/* Sin segmentacion por software. */
static inline struct sk_buff *skb_gso_segment(struct sk_buff *skb, netdev_features_t features) { return ERR_PTR(-EOPNOTSUPP); }
#endif
