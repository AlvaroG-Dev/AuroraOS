#ifndef _LKPI_LINUX_IP_H
#define _LKPI_LINUX_IP_H
#include <linux/skbuff.h>
#include <uapi/linux/ip.h>
#include <linux/udp.h>
#include <net/checksum.h>
static inline struct iphdr *ip_hdr(const struct sk_buff *skb) { return (struct iphdr *)skb_network_header(skb); }
static inline unsigned int ip_hdrlen(const struct sk_buff *skb) { return ip_hdr(skb)->ihl * 4; }
#endif
