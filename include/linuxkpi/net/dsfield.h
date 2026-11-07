#ifndef _LKPI_NET_DSFIELD_H
#define _LKPI_NET_DSFIELD_H
#include <linux/types.h>
#include <linux/ip.h>
static inline __u8 ipv4_get_dsfield(const struct iphdr *iph) { return iph->tos; }
#endif
