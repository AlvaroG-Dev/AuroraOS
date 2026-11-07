#ifndef _LKPI_NET_IP6_CHECKSUM_H
#define _LKPI_NET_IP6_CHECKSUM_H
#include <linux/skbuff.h>
#include <linux/ipv6.h>
#include <linux/ip.h>
/* La capa no genera checksums por software: el driver los hace por hardware
 * o el skb se completa aqui. */
int skb_checksum_help(struct sk_buff *skb);
static inline int skb_checksum_help_dummy(void) { return 0; }
#endif
