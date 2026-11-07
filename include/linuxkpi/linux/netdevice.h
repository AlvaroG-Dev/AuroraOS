/* SPDX-License-Identifier: GPL-2.0 */
/* netdevice.h de la capa LinuxKPI-NET (Plan_Red.md). Subconjunto de
 * struct net_device/NAPI/colas de TX que usan los drivers Ethernet PCI; la
 * glue (kernel/linuxkpi/net/netdev.c) lo conecta con netif_t de Aurora. */
#ifndef _LKPI_LINUX_NETDEVICE_H
#define _LKPI_LINUX_NETDEVICE_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/ethtool.h>
#include <linux/u64_stats_sync.h>
#include <linux/rtnetlink.h>
#include <linux/pm_runtime.h>
#include <linux/crc32.h>
#include <linux/cache.h>
#include <linux/ratelimit.h>
#include <linux/interrupt.h>
#include <linux/jump_label.h>
#include <linux/smp.h>
#include <linux/delay.h>

struct phy_device;
struct mii_bus;
struct ethtool_ops;

typedef u64 netdev_features_t;
#define NETIF_F_SG		(1ULL << 0)
#define NETIF_F_IP_CSUM		(1ULL << 1)
#define NETIF_F_HW_CSUM		(1ULL << 3)
#define NETIF_F_IPV6_CSUM	(1ULL << 4)
#define NETIF_F_HIGHDMA		(1ULL << 5)
#define NETIF_F_HW_VLAN_CTAG_TX	(1ULL << 7)
#define NETIF_F_HW_VLAN_CTAG_RX	(1ULL << 8)
#define NETIF_F_HW_VLAN_CTAG_FILTER (1ULL << 9)
#define NETIF_F_LLTX		(1ULL << 10)
#define NETIF_F_GSO		(1ULL << 11)
#define NETIF_F_TSO		(1ULL << 12)
#define NETIF_F_TSO6		(1ULL << 13)
#define NETIF_F_RXCSUM		(1ULL << 14)
#define NETIF_F_RXHASH		(1ULL << 15)
#define NETIF_F_RXFCS		(1ULL << 16)
#define NETIF_F_NOCACHE_COPY	(1ULL << 17)
#define NETIF_F_LOOPBACK	(1ULL << 18)
#define NETIF_F_GRO		(1ULL << 19)
#define NETIF_F_NTUPLE		(1ULL << 20)
#define NETIF_F_ALL_TSO		(NETIF_F_TSO | NETIF_F_TSO6)
#define NETIF_F_CSUM_MASK	(NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM | NETIF_F_HW_CSUM)
#define NETIF_F_GSO_UDP_L4	(1ULL << 21)
#define NETIF_F_GSO_PARTIAL	(1ULL << 22)
#define NETIF_F_SCTP_CRC	(1ULL << 23)
#define NETIF_F_HW_VLAN_CTAG_FILTER_ (NETIF_F_HW_VLAN_CTAG_FILTER)
#define NETIF_F_TSO_ECN		(1ULL << 24)
#define NETIF_F_GSO_UDP_TUNNEL	(1ULL << 25)
#define NETIF_F_HW_TC		(1ULL << 26)
#define NETIF_F_GSO_SOFTWARE (NETIF_F_TSO | NETIF_F_TSO6 | NETIF_F_GSO_UDP_L4)
#define NETIF_F_GSO_GRE		(1ULL << 40)
#define NETIF_F_GSO_GRE_CSUM	(1ULL << 41)
#define NETIF_F_GSO_IPXIP4	(1ULL << 42)
#define NETIF_F_GSO_IPXIP6	(1ULL << 43)
#define NETIF_F_GSO_UDP_TUNNEL_CSUM (1ULL << 44)
#define NETIF_F_RX_UDP_TUNNEL_PORT (1ULL << 27)

#define IFF_LIVE_ADDR_CHANGE 0x100000
#define IFF_UNICAST_FLT	0x20000
#define IFF_NO_QUEUE	0x40000
#define IFF_TX_SKB_SHARING 0x10000
#define IFF_SUPP_NOFCS	0x80000
#define IFF_RXFH_CONFIGURED (1<<0)
#define NET_NAME_UNKNOWN 0
#define NET_NAME_ENUM 1
#define NET_ADDR_PERM 0
#define NET_ADDR_RANDOM 1
#define NETDEV_ALIGN 32
#define MAX_ADDR_LEN 32
#define IFNAMSIZ 16
#define NAPI_POLL_WEIGHT 64
#define LL_MAX_HEADER 96
#define NETIF_MSG_DRV 0x0001
#define NETIF_MSG_PROBE 0x0002
#define NETIF_MSG_LINK 0x0004
#define NETIF_MSG_TIMER 0x0008
#define NETIF_MSG_IFDOWN 0x0010
#define NETIF_MSG_IFUP 0x0020
#define NETIF_MSG_RX_ERR 0x0040
#define NETIF_MSG_TX_ERR 0x0080
#define NETIF_MSG_TX_QUEUED 0x0100
#define NETIF_MSG_INTR 0x0200
#define NETIF_MSG_TX_DONE 0x0400
#define NETIF_MSG_RX_STATUS 0x0800
#define NETIF_MSG_PKTDATA 0x1000
#define NETIF_MSG_HW 0x2000
#define NETIF_MSG_WOL 0x4000
#define netif_msg_drv(p) ((p)->msg_enable & NETIF_MSG_DRV)
#define netif_msg_probe(p) ((p)->msg_enable & NETIF_MSG_PROBE)
#define netif_msg_link(p) ((p)->msg_enable & NETIF_MSG_LINK)
#define netif_msg_ifdown(p) ((p)->msg_enable & NETIF_MSG_IFDOWN)
#define netif_msg_ifup(p) ((p)->msg_enable & NETIF_MSG_IFUP)
#define netif_msg_rx_err(p) ((p)->msg_enable & NETIF_MSG_RX_ERR)
#define netif_msg_tx_err(p) ((p)->msg_enable & NETIF_MSG_TX_ERR)
#define netif_msg_intr(p) ((p)->msg_enable & NETIF_MSG_INTR)
#define netif_msg_hw(p) ((p)->msg_enable & NETIF_MSG_HW)
#define netif_msg_wol(p) ((p)->msg_enable & NETIF_MSG_WOL)
#define netif_msg_tx_queued(p) ((p)->msg_enable & NETIF_MSG_TX_QUEUED)
#define netif_msg_rx_status(p) ((p)->msg_enable & NETIF_MSG_RX_STATUS)
#define netif_msg_pktdata(p) ((p)->msg_enable & NETIF_MSG_PKTDATA)
#define netif_msg_tx_done(p) ((p)->msg_enable & NETIF_MSG_TX_DONE)
#define netif_msg_timer(p) ((p)->msg_enable & NETIF_MSG_TIMER)

enum netdev_tx { NETDEV_TX_OK = 0x00, NETDEV_TX_BUSY = 0x10 };
typedef enum netdev_tx netdev_tx_t;
#define NETDEV_TX_LOCKED 0x20

enum netdev_state_t { __LINK_STATE_START, __LINK_STATE_PRESENT, __LINK_STATE_NOCARRIER, __LINK_STATE_LINKWATCH_PENDING, __LINK_STATE_DORMANT };
enum { __QUEUE_STATE_DRV_XOFF, __QUEUE_STATE_STACK_XOFF, __QUEUE_STATE_FROZEN };
enum netdev_queue_stop_type { NETDEV_QSTATE_DUMMY };

struct net_device_stats {
	unsigned long rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors,
		      tx_errors, rx_dropped, tx_dropped, multicast, collisions,
		      rx_length_errors, rx_over_errors, rx_crc_errors,
		      rx_frame_errors, rx_fifo_errors, rx_missed_errors,
		      tx_aborted_errors, tx_carrier_errors, tx_fifo_errors,
		      tx_heartbeat_errors, tx_window_errors, rx_compressed,
		      tx_compressed;
};
struct pcpu_sw_netstats { u64_stats_t rx_packets, rx_bytes, tx_packets, tx_bytes; struct u64_stats_sync syncp; };

struct netdev_hw_addr {
	struct list_head list;
	unsigned char addr[MAX_ADDR_LEN];
	unsigned char type;
};
struct netdev_hw_addr_list { struct list_head list; int count; };

struct netdev_queue {
	struct net_device *dev;
	unsigned long state;
	unsigned long trans_start;
	spinlock_t _xmit_lock;
	unsigned int dql_dummy;
};

struct napi_struct {
	struct list_head poll_list;
	unsigned long state;
	int weight;
	int (*poll)(struct napi_struct *, int);
	struct net_device *dev;
	int napi_id;
	int lkpi_scheduled;
	struct tasklet_struct lkpi_tl;
	struct sk_buff *skb;
};

#define NETIF_F_RXALL (1ULL << 28)
#define NETDEV_PCPU_STAT_NONE 0
#define NETDEV_PCPU_STAT_TSTATS 1
void dev_sw_netstats_rx_add(struct net_device *dev, unsigned int len);
void dev_sw_netstats_tx_add(struct net_device *dev, unsigned int packets, unsigned int len);
void dev_fetch_sw_netstats(struct rtnl_link_stats64 *s, const struct pcpu_sw_netstats __percpu *netstats);
static inline bool __netdev_sent_queue(struct net_device *dev, unsigned int bytes, bool xmit_more) { return false; }
struct net_device_ops;
struct kernel_hwtstamp_config;
struct ifreq;

#include <net/net_namespace.h>
struct wireless_dev;
struct net_device {
	char name[IFNAMSIZ];
	struct wireless_dev *ieee80211_ptr;
	possible_net_t nd_net;
	bool netns_immutable;
	bool needs_free_netdev;
	unsigned short hard_header_len_unused;
	struct device dev;
	unsigned long state;
	netdev_features_t features, hw_features, wanted_features, vlan_features, hw_enc_features;
	unsigned int flags;
	unsigned int priv_flags;
	const struct net_device_ops *netdev_ops;
	const struct ethtool_ops *ethtool_ops;
	unsigned int mtu, min_mtu, max_mtu;
	unsigned short needed_headroom, needed_tailroom, hard_header_len;
	netdev_features_t gso_partial_features, mpls_features;
	u64 xdp_features;
	s16 num_tc;
	struct { u16 count, offset; } tc_to_txq[16];
	const struct xdp_metadata_ops *xdp_metadata_ops;
	const struct xsk_tx_metadata_ops *xsk_tx_metadata_ops;
	unsigned long mem_start, mem_end;
	unsigned char addr_len;
	unsigned char addr_assign_type;
	unsigned char dev_addr_buf[MAX_ADDR_LEN];
	unsigned char *dev_addr;
	unsigned char perm_addr[MAX_ADDR_LEN];
	unsigned char broadcast[MAX_ADDR_LEN];
	struct netdev_hw_addr_list mc;
	struct net_device_stats stats;
	unsigned int num_tx_queues, real_num_tx_queues, num_rx_queues, real_num_rx_queues;
	struct netdev_queue *_tx;
	unsigned int gso_max_size, gso_max_segs, tso_max_size, tso_max_segs;
	unsigned long irq_flags_unused;
	int irq;
	int watchdog_timeo;
	u16 type;
	u32 msg_enable_unused;
	int ifindex;
	struct phy_device *phydev;
	struct pcpu_sw_netstats __percpu *tstats;
	int pcpu_stat_type;
	struct ethtool_netdev_state *ethtool;
	bool mac_managed_pm;
	bool mac_managed_pm_unused;
	void *lkpi_netif;		/* netif_t de Aurora (netdev.c) */
	int lkpi_registered;
	void *lkpi_priv_area;
	struct work_struct lkpi_tx_timeout_work;
	struct timer_list lkpi_watchdog;
	int lkpi_carrier;
	int lkpi_running;
	bool dismantle;
	atomic_t lkpi_txq_stopped;
};

struct net_device_path_ctx; struct net_device_path;
enum tc_setup_type { TC_QUERY_CAPS = 1, TC_SETUP_QDISC_MQPRIO, TC_SETUP_QDISC_TAPRIO, TC_SETUP_QDISC_ETF, TC_SETUP_QDISC_CBS, TC_SETUP_BLOCK, TC_SETUP_FT, TC_SETUP_QDISC_FIFO };
struct xdp_frame;
struct bpf_prog;
struct xdp_metadata_ops;
struct xsk_tx_metadata_ops;
struct skb_shared_hwtstamps;
struct xsk_buff_pool;
struct netlink_ext_ack;
enum bpf_netdev_command { XDP_SETUP_PROG, XDP_SETUP_PROG_HW, BPF_OFFLOAD_MAP_ALLOC, XDP_SETUP_XSK_POOL };
struct netdev_bpf {
	enum bpf_netdev_command command;
	union {
		struct { u32 flags; struct bpf_prog *prog; struct netlink_ext_ack *extack; };
		struct { struct xsk_buff_pool *pool; u16 queue_id; } xsk;
	};
};
static inline void netdev_reset_tc(struct net_device *dev) { }
static inline int netdev_set_num_tc(struct net_device *dev, u8 num_tc) { return 0; }
static inline int netdev_set_tc_queue(struct net_device *dev, u8 tc, u16 count, u16 offset) { return 0; }
struct net_device_ops {
	int (*ndo_init)(struct net_device *dev);
	void (*ndo_uninit)(struct net_device *dev);
	int (*ndo_open)(struct net_device *dev);
	int (*ndo_stop)(struct net_device *dev);
	netdev_tx_t (*ndo_start_xmit)(struct sk_buff *skb, struct net_device *dev);
	netdev_features_t (*ndo_features_check)(struct sk_buff *skb, struct net_device *dev, netdev_features_t features);
	void (*ndo_set_rx_mode)(struct net_device *dev);
	int (*ndo_set_mac_address)(struct net_device *dev, void *addr);
	int (*ndo_validate_addr)(struct net_device *dev);
	int (*ndo_eth_ioctl)(struct net_device *dev, struct ifreq *ifr, int cmd);
	int (*ndo_siocdevprivate)(struct net_device *dev, struct ifreq *ifr, void __user *data, int cmd);
	int (*ndo_change_mtu)(struct net_device *dev, int new_mtu);
	void (*ndo_tx_timeout)(struct net_device *dev, unsigned int txqueue);
	void (*ndo_get_stats64)(struct net_device *dev, struct rtnl_link_stats64 *storage);
	struct net_device_stats *(*ndo_get_stats)(struct net_device *dev);
	int (*ndo_set_features)(struct net_device *dev, netdev_features_t features);
	netdev_features_t (*ndo_fix_features)(struct net_device *dev, netdev_features_t features);
	int (*ndo_vlan_rx_add_vid)(struct net_device *dev, __be16 proto, u16 vid);
	int (*ndo_vlan_rx_kill_vid)(struct net_device *dev, __be16 proto, u16 vid);
	void (*ndo_poll_controller)(struct net_device *dev);
	int (*ndo_hwtstamp_get)(struct net_device *dev, struct kernel_hwtstamp_config *cfg);
	int (*ndo_hwtstamp_set)(struct net_device *dev, struct kernel_hwtstamp_config *cfg, struct netlink_ext_ack *extack);
	u16 (*ndo_select_queue)(struct net_device *dev, struct sk_buff *skb, struct net_device *sb_dev);
	int (*ndo_set_vf_mac)(struct net_device *dev, int queue, u8 *mac);
	int (*ndo_set_vf_vlan)(struct net_device *dev, int queue, u16 vlan, u8 qos, __be16 proto);
	int (*ndo_set_vf_rate)(struct net_device *dev, int vf, int min_tx_rate, int max_tx_rate);
	int (*ndo_set_vf_spoofchk)(struct net_device *dev, int vf, bool setting);
	int (*ndo_set_vf_trust)(struct net_device *dev, int vf, bool setting);
	int (*ndo_get_vf_config)(struct net_device *dev, int vf, void *ivf);
	int (*ndo_set_vf_link_state)(struct net_device *dev, int vf, int link_state);
	int (*ndo_fill_forward_path)(struct net_device_path_ctx *ctx, struct net_device_path *path);
	int (*ndo_setup_tc)(struct net_device *dev, enum tc_setup_type type, void *type_data);
	int (*ndo_bpf)(struct net_device *dev, struct netdev_bpf *bpf);
	int (*ndo_xdp_xmit)(struct net_device *dev, int n, struct xdp_frame **xdp, u32 flags);
	int (*ndo_xsk_wakeup)(struct net_device *dev, u32 queue_id, u32 flags);
	ktime_t (*ndo_get_tstamp)(struct net_device *dev, const struct skb_shared_hwtstamps *hwtstamps, bool cycles);
};

/* ---- API principal ---- */
struct net_device *lkpi_alloc_netdev_mqs(int sizeof_priv, const char *name, unsigned int txqs, unsigned int rxqs);
#define alloc_netdev_mqs(sp, name, nat, setup, tx, rx) lkpi_alloc_netdev_mqs(sp, name, tx, rx)
void free_netdev(struct net_device *dev);
int register_netdev(struct net_device *dev);
void unregister_netdev(struct net_device *dev);
#define devm_alloc_etherdev(dev, sp) devm_alloc_etherdev_mqs(dev, sp, 1, 1)
struct net_device *devm_alloc_etherdev_mqs(struct device *dev, int sizeof_priv, unsigned int txqs, unsigned int rxqs);
int devm_register_netdev(struct device *dev, struct net_device *ndev);
static inline void *netdev_priv(const struct net_device *dev)
{ return (char *)dev + ALIGN(sizeof(struct net_device), NETDEV_ALIGN); }
#define SET_NETDEV_DEV(net, pdev) ((net)->dev.parent = (pdev))
#define SET_NETDEV_DEVTYPE(net, devtype) do { } while (0)
static inline struct net_device *to_net_dev(struct device *d) { return container_of(d, struct net_device, dev); }

int netif_rx(struct sk_buff *skb);
int netif_receive_skb(struct sk_buff *skb);
typedef enum { GRO_MERGED, GRO_MERGED_FREE, GRO_HELD, GRO_NORMAL, GRO_CONSUMED } gro_result_t;
gro_result_t napi_gro_receive(struct napi_struct *napi, struct sk_buff *skb);
struct sk_buff *napi_alloc_skb(struct napi_struct *napi, unsigned int length);
struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int length, gfp_t gfp);
static inline struct sk_buff *netdev_alloc_skb(struct net_device *dev, unsigned int length)
{ return __netdev_alloc_skb(dev, length, GFP_ATOMIC); }
static inline struct sk_buff *netdev_alloc_skb_ip_align(struct net_device *dev, unsigned int length)
{
	struct sk_buff *skb = __netdev_alloc_skb(dev, length + NET_IP_ALIGN, GFP_ATOMIC);
	if (skb) skb_reserve(skb, NET_IP_ALIGN);
	return skb;
}
static inline struct sk_buff *napi_alloc_skb_ip_align(struct napi_struct *n, unsigned int length)
{ return netdev_alloc_skb_ip_align(n->dev, length); }
static inline struct sk_buff *dev_alloc_skb(unsigned int length) { return alloc_skb(length + NET_SKB_PAD, GFP_ATOMIC); }

/* NAPI */
static inline int netif_set_real_num_queues(struct net_device *dev, unsigned int txq, unsigned int rxq)
{ dev->real_num_tx_queues = txq; dev->real_num_rx_queues = rxq; return 0; }
static inline bool __netif_subqueue_stopped(const struct net_device *dev, u16 q) { return test_bit(__QUEUE_STATE_DRV_XOFF, &dev->_tx[q].state); }
static inline int __dev_uc_sync(struct net_device *dev, int (*sync)(struct net_device *, const unsigned char *), int (*unsync)(struct net_device *, const unsigned char *)) { return 0; }
static inline void __dev_uc_unsync(struct net_device *dev, int (*unsync)(struct net_device *, const unsigned char *)) { }
static inline int __dev_mc_sync(struct net_device *dev, int (*sync)(struct net_device *, const unsigned char *), int (*unsync)(struct net_device *, const unsigned char *)) { return 0; }
static inline void __dev_mc_unsync(struct net_device *dev, int (*unsync)(struct net_device *, const unsigned char *)) { }
#define NETIF_F_TSO_MANGLEID (1ULL << 29)
#define netdev_err_once(dev, fmt, ...) netdev_warn_once(dev, fmt, ##__VA_ARGS__)
static inline void __netif_tx_lock(struct netdev_queue *txq, int cpu) { }
static inline void __netif_tx_unlock(struct netdev_queue *txq) { }
static inline void __netif_tx_lock_bh(struct netdev_queue *txq) { }
static inline void __netif_tx_unlock_bh(struct netdev_queue *txq) { }
#define NETDEV_QUEUE_TYPE_RX 0
#define NETDEV_QUEUE_TYPE_TX 1
static inline void netif_napi_set_irq(struct napi_struct *napi, int irq) { }
static inline void netif_queue_set_napi(struct net_device *dev, unsigned int q_idx, int type, struct napi_struct *napi) { }
static inline unsigned long dev_trans_start(struct net_device *dev) { return jiffies; }
static inline void netdev_rss_key_fill(void *buffer, size_t len) { memset(buffer, 0x5a, len); }
static inline struct sk_buff *__netdev_alloc_skb_ip_align(struct net_device *dev, unsigned int length, gfp_t gfp)
{
	struct sk_buff *skb = __netdev_alloc_skb(dev, length + NET_IP_ALIGN, gfp);
	if (skb) skb_reserve(skb, NET_IP_ALIGN);
	return skb;
}
void netif_napi_add_weight(struct net_device *dev, struct napi_struct *napi, int (*poll)(struct napi_struct *, int), int weight);
#define netif_napi_add(dev, napi, poll) netif_napi_add_weight(dev, napi, poll, NAPI_POLL_WEIGHT)
#define netif_napi_add_tx(dev, napi, poll) netif_napi_add_weight(dev, napi, poll, NAPI_POLL_WEIGHT)
#define netif_napi_add_config(dev, napi, poll, idx) netif_napi_add_weight(dev, napi, poll, NAPI_POLL_WEIGHT)
void netif_napi_del(struct napi_struct *napi);
void napi_enable(struct napi_struct *n);
void napi_disable(struct napi_struct *n);
bool napi_schedule_prep(struct napi_struct *n);
static inline bool napi_if_scheduled_mark_missed(struct napi_struct *n) { return false; }
void __napi_schedule(struct napi_struct *n);
static inline bool napi_schedule(struct napi_struct *n) { if (napi_schedule_prep(n)) { __napi_schedule(n); return true; } return false; }
static inline void napi_schedule_irqoff(struct napi_struct *n) { napi_schedule(n); }
static inline void __napi_schedule_irqoff(struct napi_struct *n) { __napi_schedule(n); }
bool napi_complete_done(struct napi_struct *n, int work_done);
static inline bool napi_complete(struct napi_struct *n) { return napi_complete_done(n, 0); }
static inline void napi_synchronize(const struct napi_struct *n) { }
static inline bool napi_is_scheduled(struct napi_struct *n) { return n->lkpi_scheduled; }
static inline void netdev_sw_irq_coalesce_default_on(struct net_device *dev) { }
static inline void netif_set_tso_max_size(struct net_device *dev, unsigned int size) { dev->tso_max_size = size; }
static inline void netif_set_tso_max_segs(struct net_device *dev, unsigned int segs) { dev->tso_max_segs = segs; }
static inline void netif_set_gso_max_size(struct net_device *dev, unsigned int size) { dev->gso_max_size = size; }

/* colas de TX */
static inline struct netdev_queue *netdev_get_tx_queue(const struct net_device *dev, unsigned int index) { return &dev->_tx[index]; }
static inline struct netdev_queue *skb_get_tx_queue(const struct net_device *dev, const struct sk_buff *skb)
{ return netdev_get_tx_queue(dev, skb_get_queue_mapping(skb)); }
void netif_tx_stop_queue_(struct net_device *dev, struct netdev_queue *q);
void netif_tx_wake_queue_(struct net_device *dev, struct netdev_queue *q);
static inline void netif_tx_start_queue(struct netdev_queue *q) { clear_bit(__QUEUE_STATE_DRV_XOFF, &q->state); }
static inline void netif_start_queue(struct net_device *dev) { netif_tx_start_queue(netdev_get_tx_queue(dev, 0)); }
static inline void netif_tx_stop_queue(struct netdev_queue *q) { set_bit(__QUEUE_STATE_DRV_XOFF, &q->state); }
static inline void netif_stop_queue(struct net_device *dev) { netif_tx_stop_queue(netdev_get_tx_queue(dev, 0)); }
static inline bool netif_tx_queue_stopped(const struct netdev_queue *q) { return test_bit(__QUEUE_STATE_DRV_XOFF, &q->state); }
static inline bool netif_xmit_stopped(const struct netdev_queue *q) { return q->state & ((1UL << __QUEUE_STATE_DRV_XOFF) | (1UL << __QUEUE_STATE_STACK_XOFF)); }
static inline netdev_features_t passthru_features_check(struct sk_buff *skb, struct net_device *dev, netdev_features_t features) { return features; }
static inline u32 netif_msg_init(int debug_value, int default_msg_enable_bits)
{ if (debug_value < 0 || debug_value >= 32) return default_msg_enable_bits; if (debug_value == 0) return 0; return (1U << debug_value) - 1; }
static inline bool netif_queue_stopped(const struct net_device *dev) { return netif_tx_queue_stopped(netdev_get_tx_queue(dev, 0)); }
void netif_tx_wake_queue(struct netdev_queue *q);
static inline void netif_wake_queue(struct net_device *dev) { netif_tx_wake_queue(netdev_get_tx_queue(dev, 0)); }
static inline void netif_wake_subqueue(struct net_device *dev, u16 q) { netif_tx_wake_queue(netdev_get_tx_queue(dev, q)); }
static inline void netif_stop_subqueue(struct net_device *dev, u16 q) { netif_tx_stop_queue(netdev_get_tx_queue(dev, q)); }
static inline bool netif_subqueue_stopped(const struct net_device *dev, u16 q) { return netif_tx_queue_stopped(netdev_get_tx_queue(dev, q)); }
static inline void netif_start_subqueue(struct net_device *dev, u16 q) { netif_tx_start_queue(netdev_get_tx_queue(dev, q)); }
static inline void netif_tx_disable(struct net_device *dev) { netif_tx_stop_queue(netdev_get_tx_queue(dev, 0)); }
static inline void netif_tx_lock_bh(struct net_device *dev) { }
static inline void netif_tx_unlock_bh(struct net_device *dev) { }
static inline void netif_trans_update(struct net_device *dev) { }
static inline void txq_trans_cond_update(struct netdev_queue *q) { }
static inline void netdev_tx_sent_queue(struct netdev_queue *q, unsigned int bytes) { }
static inline void netdev_tx_completed_queue(struct netdev_queue *q, unsigned int pkts, unsigned int bytes) { }
static inline void netdev_completed_queue(struct net_device *dev, unsigned int pkts, unsigned int bytes) { }
static inline void netdev_sent_queue(struct net_device *dev, unsigned int bytes) { }
static inline void netdev_tx_reset_queue(struct netdev_queue *q) { }
static inline void netdev_reset_queue(struct net_device *dev) { }
static inline void netdev_tx_reset_subqueue(const struct net_device *dev, u32 qid) { }
static inline bool netdev_xmit_more(void) { return false; }
static inline bool __netdev_tx_sent_queue(struct netdev_queue *q, unsigned int bytes, bool xmit_more) { return false; }
static inline void netdev_queue_set_dql_min_limit(struct netdev_queue *q, unsigned int m) { }
int netif_set_real_num_tx_queues(struct net_device *dev, unsigned int txq);
int netif_set_real_num_rx_queues(struct net_device *dev, unsigned int rxq);

/* estado */
static inline bool netif_running(const struct net_device *dev) { return test_bit(__LINK_STATE_START, &dev->state); }
static inline bool netif_device_present(const struct net_device *dev) { return test_bit(__LINK_STATE_PRESENT, &dev->state); }
static inline bool netif_carrier_ok(const struct net_device *dev) { return !test_bit(__LINK_STATE_NOCARRIER, &dev->state); }
static inline bool netif_oper_up(const struct net_device *dev) { return netif_carrier_ok(dev); }
static inline bool netif_dormant(const struct net_device *dev) { return false; }
void netif_carrier_on(struct net_device *dev);
void netif_carrier_off(struct net_device *dev);
static inline void netif_carrier_event(struct net_device *dev) { }
void netif_device_detach(struct net_device *dev);
void netif_device_attach(struct net_device *dev);
static inline void netif_tx_start_all_queues(struct net_device *dev) { unsigned int i; for (i = 0; i < dev->num_tx_queues; i++) netif_tx_start_queue(&dev->_tx[i]); }
static inline void netif_tx_stop_all_queues(struct net_device *dev) { unsigned int i; for (i = 0; i < dev->num_tx_queues; i++) netif_tx_stop_queue(&dev->_tx[i]); }
static inline void netif_tx_wake_all_queues(struct net_device *dev) { unsigned int i; for (i = 0; i < dev->num_tx_queues; i++) netif_tx_wake_queue(&dev->_tx[i]); }
void netdev_update_features(struct net_device *dev);
static inline void netdev_change_features(struct net_device *dev) { netdev_update_features(dev); }
void netdev_stats_to_stats64(struct rtnl_link_stats64 *stats64, const struct net_device_stats *netdev_stats);
int dev_open(struct net_device *dev, void *extack);
void dev_close(struct net_device *dev);
static inline int netdev_mc_count(const struct net_device *dev) { return dev->mc.count; }
#define netdev_mc_empty(dev) (netdev_mc_count(dev) == 0)
#define netdev_for_each_mc_addr(ha, dev) list_for_each_entry(ha, &(dev)->mc.list, list)
static inline int netdev_uc_count(const struct net_device *dev) { return 0; }
#define netdev_uc_empty(dev) 1
#define netdev_for_each_uc_addr(ha, dev) for (ha = NULL; ha; )
static inline bool netdev_uses_dsa(const struct net_device *dev) { return false; }
static inline int dev_get_flags_dummy(void) { return 0; }
static inline unsigned long netdev_get_features(void) { return 0; }
static inline void netdev_features_set_dummy(void) { }
static inline bool net_ratelimit(void) { return true; }
static inline int netdev_set_default_ethtool_ops_dummy(void) { return 0; }
static inline void dev_hold(struct net_device *d) { }
static inline void dev_put(struct net_device *d) { }
static inline unsigned int dev_get_mac_address_dummy(void) { return 0; }
static inline bool dev_is_pci_dummy(void) { return false; }

/* mensajes */
const char *netdev_name(const struct net_device *dev);
#define netdev_printk(lvl, dev, fmt, ...) dev_printk(lvl, &(dev)->dev, "%s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_emerg(dev, fmt, ...) printk(KERN_EMERG "%s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_alert(dev, fmt, ...) printk(KERN_ALERT "%s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_crit(dev, fmt, ...) printk(KERN_CRIT "%s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_err(dev, fmt, ...) printk(KERN_ERR "%s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_warn(dev, fmt, ...) printk(KERN_WARNING "%s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_notice(dev, fmt, ...) printk(KERN_NOTICE "%s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_info(dev, fmt, ...) printk(KERN_INFO "%s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_dbg(dev, fmt, ...) do { if (0) printk(KERN_DEBUG "%s: " fmt, (dev)->name, ##__VA_ARGS__); } while (0)
#define netdev_warn_once(dev, fmt, ...) ({ static bool __once; if (!__once) { __once = true; netdev_warn(dev, fmt, ##__VA_ARGS__); } })
#define netdev_notice_once(dev, fmt, ...) ({ static bool __once; if (!__once) { __once = true; netdev_notice(dev, fmt, ##__VA_ARGS__); } })
#define netif_printk(priv, type, lvl, dev, fmt, ...) do { if (netif_msg_##type(priv)) netdev_printk(lvl, dev, fmt, ##__VA_ARGS__); } while (0)
#define netif_err(priv, type, dev, fmt, ...) do { if (netif_msg_##type(priv)) netdev_err(dev, fmt, ##__VA_ARGS__); } while (0)
#define netif_warn(priv, type, dev, fmt, ...) do { if (netif_msg_##type(priv)) netdev_warn(dev, fmt, ##__VA_ARGS__); } while (0)
#define netif_notice(priv, type, dev, fmt, ...) do { if (netif_msg_##type(priv)) netdev_notice(dev, fmt, ##__VA_ARGS__); } while (0)
#define netif_info(priv, type, dev, fmt, ...) do { if (netif_msg_##type(priv)) netdev_info(dev, fmt, ##__VA_ARGS__); } while (0)
#define netif_dbg(priv, type, dev, fmt, ...) do { } while (0)
#define netif_level(l, priv, type, dev, fmt, ...) do { } while (0)

static inline struct rtnl_link_stats64 *dev_get_stats(struct net_device *dev, struct rtnl_link_stats64 *storage)
{
	if (dev->netdev_ops->ndo_get_stats64) { memset(storage, 0, sizeof(*storage)); dev->netdev_ops->ndo_get_stats64(dev, storage); }
	else if (dev->netdev_ops->ndo_get_stats) netdev_stats_to_stats64(storage, dev->netdev_ops->ndo_get_stats(dev));
	else netdev_stats_to_stats64(storage, &dev->stats);
	return storage;
}
#define module_pci_driver_dummy

/* ---- un solo espacio de nombres y notificadores de dispositivo (cfg80211) ---- */
#include <linux/notifier.h>
static inline struct net *dev_net(const struct net_device *dev) { return read_pnet(&dev->nd_net); }
static inline void dev_net_set(struct net_device *dev, struct net *net) { write_pnet(&dev->nd_net, net); }
enum netdev_cmd { NETDEV_UP = 1, NETDEV_DOWN, NETDEV_REBOOT, NETDEV_CHANGE, NETDEV_REGISTER, NETDEV_UNREGISTER, NETDEV_CHANGEMTU,
	NETDEV_CHANGEADDR, NETDEV_PRE_CHANGEADDR, NETDEV_GOING_DOWN, NETDEV_CHANGENAME, NETDEV_FEAT_CHANGE, NETDEV_BONDING_FAILOVER,
	NETDEV_PRE_UP, NETDEV_PRE_TYPE_CHANGE, NETDEV_POST_TYPE_CHANGE, NETDEV_POST_INIT, NETDEV_PRE_UNINIT, NETDEV_RELEASE, NETDEV_NOTIFY_PEERS };
struct netdev_notifier_info { struct net_device *dev; struct netlink_ext_ack *extack; };
static inline struct net_device *netdev_notifier_info_to_dev(const struct netdev_notifier_info *info) { return info->dev; }
int register_netdevice_notifier(struct notifier_block *nb);
int unregister_netdevice_notifier(struct notifier_block *nb);
int call_netdevice_notifiers(unsigned long val, struct net_device *dev);
int register_netdevice(struct net_device *dev);
void unregister_netdevice(struct net_device *dev);
#define unregister_netdevice_queue(dev, head) unregister_netdevice(dev)
struct net_device *dev_get_by_index(struct net *net, int ifindex);
struct net_device *__dev_get_by_index(struct net *net, int ifindex);
#define IFF_DONT_BRIDGE 0x800
static inline bool netif_is_bridge_port(const struct net_device *dev) { return false; }
static inline int dev_change_net_namespace(struct net_device *dev, struct net *net, const char *pat) { return net == &init_net ? 0 : -EOPNOTSUPP; }
static inline void dev_set_uevent_suppress(struct device *dev, int val) { }
#define net_dbg_ratelimited(fmt, ...) do { } while (0)
#define __net_init
#define __net_exit
#include <linux/percpu.h>
struct net_device_path { int type; };
enum net_device_path_type { DEV_PATH_ETHERNET = 0, DEV_PATH_VLAN, DEV_PATH_BRIDGE, DEV_PATH_PPPOE, DEV_PATH_DSA, DEV_PATH_MTK_WDMA };
struct net_device_path_ctx { const struct net_device *dev; u8 daddr[6]; int num_vlans; };
static inline void netif_addr_lock_bh(struct net_device *dev) { }
static inline void netif_addr_unlock_bh(struct net_device *dev) { }
static inline void netif_receive_skb_list(struct list_head *head)
{ struct sk_buff *skb, *n; list_for_each_entry_safe(skb, n, head, list) { list_del(&skb->list); netif_receive_skb(skb); } }
void ether_setup(struct net_device *dev);
int dev_queue_xmit(struct sk_buff *skb);
int dev_alloc_name(struct net_device *dev, const char *name);
static inline void netdev_set_default_ethtool_ops(struct net_device *dev, const struct ethtool_ops *ops) { if (!dev->ethtool_ops) dev->ethtool_ops = ops; }
void kfree_skb_list(struct sk_buff *segs);
void skb_complete_wifi_ack(struct sk_buff *skb, bool acked);
/* listas de direcciones: sin sincronizacion de multicast */
static inline void __hw_addr_init(struct netdev_hw_addr_list *list) { INIT_LIST_HEAD(&list->list); list->count = 0; }
static inline int __hw_addr_sync(struct netdev_hw_addr_list *to_list, struct netdev_hw_addr_list *from_list, int addr_len) { return 0; }
static inline void __hw_addr_unsync(struct netdev_hw_addr_list *to_list, struct netdev_hw_addr_list *from_list, int addr_len) { }
static inline struct net_device *alloc_netdev_dummy(int sizeof_priv) { return lkpi_alloc_netdev_mqs(sizeof_priv, "dummy", 1, 1); }
#include <linux/in6.h>
#define netdev_hw_addr_list_count(l) ((l)->count)
#define netdev_hw_addr_list_for_each(ha, l) list_for_each_entry(ha, &(l)->list, list)
void lkpi_netif_wifi_link(struct net_device *dev, bool up);
#endif
