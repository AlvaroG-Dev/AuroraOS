#ifndef _LKPI_NET_XDP_SOCK_DRV_H
#define _LKPI_NET_XDP_SOCK_DRV_H
#include <net/xdp.h>
#define XSK_CHECK_PRIV_TYPE(t) do { } while (0)
#define XSK_TX_COMPL_FITS(t) do { } while (0)
struct xsk_buff_pool;
struct xdp_buff_xsk;
static inline bool xsk_uses_need_wakeup(struct xsk_buff_pool *p) { return false; }
static inline void xsk_set_rx_need_wakeup(struct xsk_buff_pool *p) { }
static inline void xsk_set_tx_need_wakeup(struct xsk_buff_pool *p) { }
static inline void xsk_clear_rx_need_wakeup(struct xsk_buff_pool *p) { }
static inline void xsk_tx_completed(struct xsk_buff_pool *p, u32 n) { }
static inline void xsk_tx_release(struct xsk_buff_pool *p) { }
static inline bool xsk_tx_peek_desc(struct xsk_buff_pool *p, struct xdp_desc *d) { return false; }
static inline struct xsk_buff_pool *xsk_get_pool_from_qid(struct net_device *dev, u16 q) { return NULL; }
static inline u16 xsk_pool_get_rx_frame_size(struct xsk_buff_pool *p) { return 0; }
static inline void xsk_pool_set_rxq_info(struct xsk_buff_pool *p, struct xdp_rxq_info *r) { }
static inline int xsk_pool_dma_map(struct xsk_buff_pool *p, struct device *dev, unsigned long attrs) { return -EOPNOTSUPP; }
static inline void xsk_pool_dma_unmap(struct xsk_buff_pool *p, unsigned long attrs) { }
static inline struct xdp_buff *xsk_buff_alloc(struct xsk_buff_pool *p) { return NULL; }
static inline void xsk_buff_free(struct xdp_buff *x) { }
static inline dma_addr_t xsk_buff_xdp_get_dma(struct xdp_buff *x) { return 0; }
static inline dma_addr_t xsk_buff_raw_get_dma(struct xsk_buff_pool *p, u64 a) { return 0; }
static inline void xsk_buff_raw_dma_sync_for_device(struct xsk_buff_pool *p, dma_addr_t d, size_t s) { }
static inline void xsk_buff_dma_sync_for_cpu(struct xdp_buff *x) { }
static inline void *xsk_buff_raw_get_data(struct xsk_buff_pool *p, u64 a) { return NULL; }
static inline struct xsk_tx_metadata *xsk_buff_get_metadata(struct xsk_buff_pool *p, u64 a, void *o) { return NULL; }
static inline void *xsk_buff_raw_get_ctx_dummy(void) { return NULL; }
static inline void xsk_tx_metadata_request(const struct xsk_buff_pool *p, struct xsk_tx_metadata **m, const struct xsk_tx_metadata_ops *o, void *priv) { }
static inline void xsk_tx_metadata_to_compl(struct xsk_tx_metadata *m, struct xsk_tx_metadata_compl *c) { }
static inline void xsk_tx_metadata_complete(struct xsk_tx_metadata_compl *c, const struct xsk_tx_metadata_ops *o, void *priv) { }
static inline u32 xsk_queue_index_dummy(void) { return 0; }
#endif
