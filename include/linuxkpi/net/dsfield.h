#ifndef _LKPI_NET_DSFIELD_H
#define _LKPI_NET_DSFIELD_H
#include <linux/types.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
static inline __u8 ipv4_get_dsfield(const struct iphdr *iph) { return iph->tos; }
static inline __u8 ipv6_get_dsfield(const struct ipv6hdr *ipv6h) { return ntohs(*(const __be16 *)ipv6h) >> 4; }
#endif
