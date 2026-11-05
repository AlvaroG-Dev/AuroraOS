#ifndef _LKPI_LINUX_IPV6_H
#define _LKPI_LINUX_IPV6_H
#include <linux/skbuff.h>
#include <uapi/linux/ipv6.h>
static inline struct ipv6hdr *ipv6_hdr(const struct sk_buff *skb) { return (struct ipv6hdr *)skb_network_header(skb); }
#endif
