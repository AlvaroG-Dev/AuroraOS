/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LKPI_LINUX_ETHERDEVICE_H
#define _LKPI_LINUX_ETHERDEVICE_H
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/random.h>
#include <linux/crc32.h>
#include <linux/string.h>

static inline bool is_zero_ether_addr(const u8 *addr) { return !(addr[0] | addr[1] | addr[2] | addr[3] | addr[4] | addr[5]); }
static inline bool is_multicast_ether_addr(const u8 *addr) { return 0x01 & addr[0]; }
static inline bool is_broadcast_ether_addr(const u8 *addr) { return (addr[0] & addr[1] & addr[2] & addr[3] & addr[4] & addr[5]) == 0xff; }
static inline bool is_unicast_ether_addr(const u8 *addr) { return !is_multicast_ether_addr(addr); }
static inline bool is_valid_ether_addr(const u8 *addr)
{ return !is_multicast_ether_addr(addr) && !is_zero_ether_addr(addr); }
static inline bool ether_addr_equal(const u8 *a, const u8 *b) { return !memcmp(a, b, ETH_ALEN); }
static inline void ether_addr_copy(u8 *dst, const u8 *src) { memcpy(dst, src, ETH_ALEN); }
static inline void eth_zero_addr(u8 *addr) { memset(addr, 0x00, ETH_ALEN); }
static inline void eth_broadcast_addr(u8 *addr) { memset(addr, 0xff, ETH_ALEN); }
static inline void eth_random_addr(u8 *addr)
{
	get_random_bytes(addr, ETH_ALEN);
	addr[0] &= 0xfe; addr[0] |= 0x02;
}
static inline void eth_hw_addr_random(struct net_device *dev)
{ u8 a[ETH_ALEN]; eth_random_addr(a); memcpy(dev->dev_addr, a, ETH_ALEN); dev->addr_assign_type = 1; }
static inline void eth_hw_addr_set(struct net_device *dev, const u8 *addr) { memcpy(dev->dev_addr, addr, ETH_ALEN); }
static inline u32 ether_crc(int length, const unsigned char *p)
{
	u32 crc = ~0U; int bit;
	while (--length >= 0) {
		unsigned char c = *p++;
		for (bit = 0; bit < 8; bit++, c >>= 1)
			crc = (crc << 1) ^ ((((crc >> 31) ^ c) & 1) ? 0x04c11db7 : 0);
	}
	return crc;
}
static inline u32 eth_hw_addr_crc(struct netdev_hw_addr *ha) { return ether_crc(ETH_ALEN, ha->addr); }
static inline struct ethhdr *eth_hdr(const struct sk_buff *skb) { return (struct ethhdr *)skb_mac_header(skb); }
static inline bool eth_proto_is_802_3(__be16 proto)
{ return ntohs(proto) >= ETH_P_802_3_MIN; }
__be16 eth_type_trans(struct sk_buff *skb, struct net_device *dev);
int eth_mac_addr(struct net_device *dev, void *p);
int eth_validate_addr(struct net_device *dev);
int eth_platform_get_mac_address(struct device *dev, u8 *mac_addr);
struct net_device *alloc_etherdev_mqs(int sizeof_priv, unsigned int txqs, unsigned int rxqs);
#define alloc_etherdev(sizeof_priv) alloc_etherdev_mq(sizeof_priv, 1)
#define alloc_etherdev_mq(sizeof_priv, count) alloc_etherdev_mqs(sizeof_priv, count, count)
#endif
