#ifndef _LKPI_NET_INET_ECN_H
#define _LKPI_NET_INET_ECN_H
#include <linux/skbuff.h>
/* Sin marcado ECN: el codel de mac80211 descarta en lugar de marcar. */
static inline int INET_ECN_set_ce(struct sk_buff *skb) { return 0; }
static inline int skb_get_dsfield(struct sk_buff *skb) { return -1; }
#endif
