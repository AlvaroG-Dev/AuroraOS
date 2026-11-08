#ifndef _LKPI_LINUX_UDP_H
#define _LKPI_LINUX_UDP_H
#include <linux/skbuff.h>
#include <uapi/linux/udp.h>
static inline struct udphdr *udp_hdr(const struct sk_buff *skb) { return (struct udphdr *)skb_transport_header(skb); }
#endif
