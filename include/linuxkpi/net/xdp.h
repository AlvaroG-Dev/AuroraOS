/* XDP: no soportado. Tipos minimos para que compile igc; bpf_prog siempre NULL. */
#ifndef _LKPI_NET_XDP_H
#define _LKPI_NET_XDP_H
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#define XDP_PACKET_HEADROOM 256
#define XDP_PASS 2
#define XDP_ABORTED 0
#define XDP_DROP 1
#define XDP_TX 3
#define XDP_REDIRECT 4
#define XDP_XMIT_FLUSH (1U << 0)
#define XDP_XMIT_FLAGS_MASK XDP_XMIT_FLUSH
#define XDP_WAKEUP_RX (1 << 0)
#define XDP_WAKEUP_TX (1 << 1)
enum xdp_rss_hash_type { XDP_RSS_L3_IPV4 = 1, XDP_RSS_L3_IPV6 = 2, XDP_RSS_L3_DYNHDR = 4, XDP_RSS_L4 = 8, XDP_RSS_L4_TCP = 16, XDP_RSS_L4_UDP = 32, XDP_RSS_L4_SCTP = 64, XDP_RSS_L4_IPSEC = 128, XDP_RSS_L4_ICMP = 256,
	XDP_RSS_TYPE_NONE = 0, XDP_RSS_TYPE_L2 = 0, XDP_RSS_TYPE_L3_IPV4 = 1, XDP_RSS_TYPE_L3_IPV6 = 2, XDP_RSS_TYPE_L3_IPV6_EX = 6,
	XDP_RSS_TYPE_L4_IPV4_TCP = 25, XDP_RSS_TYPE_L4_IPV4_UDP = 41, XDP_RSS_TYPE_L4_IPV6_TCP = 26, XDP_RSS_TYPE_L4_IPV6_UDP = 42,
	XDP_RSS_TYPE_L4_IPV6_TCP_EX = 30, XDP_RSS_TYPE_L4_IPV6_UDP_EX = 46 };
enum xdp_mem_type { MEM_TYPE_PAGE_SHARED = 0, MEM_TYPE_PAGE_ORDER0, MEM_TYPE_PAGE_POOL, MEM_TYPE_XSK_BUFF_POOL };
struct xdp_mem_info { u32 type; u32 id; };
struct bpf_prog;
struct xdp_rxq_info { struct net_device *dev; u32 queue_index; u32 reg_state; struct xdp_mem_info mem; unsigned int napi_id; u32 frag_size; };
struct xdp_buff { void *data; void *data_end; void *data_meta; void *data_hard_start; struct xdp_rxq_info *rxq; struct xdp_txq_info *txq; u32 frame_sz; u32 flags; };
struct xdp_frame { void *data; u32 len; u32 headroom; u32 metasize; u32 frame_sz; u32 flags; struct xdp_mem_info mem; struct net_device *dev_rx; };
struct xdp_md { __u32 data, data_end, data_meta, ingress_ifindex, rx_queue_index, egress_ifindex; };
struct xdp_metadata_ops { int (*xmo_rx_timestamp)(const struct xdp_md *ctx, u64 *timestamp); int (*xmo_rx_hash)(const struct xdp_md *ctx, u32 *hash, enum xdp_rss_hash_type *rss_type); };
struct xdp_desc { u64 addr; u32 len; u32 options; };
struct skb_shared_info;
struct xsk_tx_metadata_ops { void (*tmo_request_timestamp)(void *priv); u64 (*tmo_fill_timestamp)(void *priv); void (*tmo_request_launch_time)(u64 lt, void *priv); };
struct xsk_tx_metadata { u64 flags; u64 launch_time; };
struct xsk_tx_metadata_compl { u64 *tx_timestamp; };
static inline void xdp_init_buff(struct xdp_buff *xdp, u32 frame_sz, struct xdp_rxq_info *rxq) { xdp->frame_sz = frame_sz; xdp->rxq = rxq; xdp->flags = 0; }
static inline void xdp_prepare_buff(struct xdp_buff *xdp, unsigned char *hard_start, int headroom, int data_len, bool meta_valid)
{ xdp->data_hard_start = hard_start; xdp->data = hard_start + headroom; xdp->data_end = xdp->data + data_len; xdp->data_meta = meta_valid ? xdp->data : xdp->data + 1; }
static inline void xdp_buff_clear_frags_flag(struct xdp_buff *xdp) { xdp->flags = 0; }
static inline int xdp_rxq_info_reg(struct xdp_rxq_info *r, struct net_device *dev, u32 q, unsigned int napi) { return 0; }
static inline void xdp_rxq_info_unreg(struct xdp_rxq_info *r) { }
static inline bool xdp_rxq_info_is_reg(struct xdp_rxq_info *r) { return false; }
static inline int xdp_rxq_info_reg_mem_model(struct xdp_rxq_info *r, enum xdp_mem_type t, void *id) { return 0; }
static inline void xdp_rxq_info_unreg_mem_model(struct xdp_rxq_info *r) { }
static inline void xdp_return_frame(struct xdp_frame *f) { }
static inline bool xdp_frame_has_frags(struct xdp_frame *f) { return false; }
static inline struct skb_shared_info *xdp_get_shared_info_from_frame(struct xdp_frame *f) { return NULL; }
static inline unsigned int xdp_get_frame_len(struct xdp_frame *f) { return f->len; }
static inline struct xdp_frame *xdp_convert_buff_to_frame(struct xdp_buff *xdp) { return NULL; }
static inline void xdp_do_flush(void) { }
static inline int xdp_do_redirect(struct net_device *d, struct xdp_buff *x, struct bpf_prog *p) { return -EOPNOTSUPP; }
static inline u32 bpf_prog_run_xdp(const struct bpf_prog *prog, struct xdp_buff *xdp) { return XDP_PASS; }
static inline void bpf_warn_invalid_xdp_action(struct net_device *dev, struct bpf_prog *prog, u32 act) { }
#define trace_xdp_exception(dev, prog, act) do { } while (0)
static inline void bpf_prog_put(struct bpf_prog *prog) { }
#define NETDEV_XDP_ACT_BASIC 1
#define NETDEV_XDP_ACT_REDIRECT 2
#define NETDEV_XDP_ACT_NDO_XMIT 4
#define NETDEV_XDP_ACT_RX_SG 8
#define NETDEV_XDP_ACT_NDO_XMIT_SG 16
#define NETDEV_XDP_ACT_XSK_ZEROCOPY 32
static inline void xdp_set_features_flag(struct net_device *dev, u64 f) { }
static inline void xdp_features_set_redirect_target(struct net_device *dev, bool support_sg) { }
static inline void xdp_features_clear_redirect_target(struct net_device *dev) { }
#endif
