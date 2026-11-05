#ifndef _LKPI_LINUX_IF_VLAN_H
#define _LKPI_LINUX_IF_VLAN_H
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <uapi/linux/if_vlan.h>
#define VLAN_HLEN 4
#define VLAN_ETH_HLEN 18
#define VLAN_ETH_ZLEN 64
#define VLAN_VID_MASK 0x0fff
#define VLAN_N_VID 4096
#define VLAN_CFI_MASK 0x1000
#define VLAN_PRIO_MASK 0xe000
#define VLAN_PRIO_SHIFT 13
struct vlan_hdr { __be16 h_vlan_TCI; __be16 h_vlan_encapsulated_proto; };
struct vlan_ethhdr { unsigned char h_dest[ETH_ALEN]; unsigned char h_source[ETH_ALEN]; __be16 h_vlan_proto; __be16 h_vlan_TCI; __be16 h_vlan_encapsulated_proto; };
static inline __be16 vlan_get_protocol(const struct sk_buff *skb) { return skb->protocol; }
static inline netdev_features_t vlan_features_check(struct sk_buff *skb, netdev_features_t features) { return features; }
#endif
