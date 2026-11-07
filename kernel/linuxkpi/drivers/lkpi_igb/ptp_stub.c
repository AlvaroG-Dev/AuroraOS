// SPDX-License-Identifier: GPL-2.0
/* igb sin PTP (igb_ptp.c): el reloj de hardware y las marcas de tiempo no se usan. */
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include "igb.h"

void igb_ptp_init(struct igb_adapter *adapter) { }
void igb_ptp_stop(struct igb_adapter *adapter) { }
void igb_ptp_reset(struct igb_adapter *adapter) { }
void igb_ptp_suspend(struct igb_adapter *adapter) { }
void igb_ptp_rx_hang(struct igb_adapter *adapter) { }
void igb_ptp_tx_hang(struct igb_adapter *adapter) { }
void igb_ptp_rx_rgtstamp(struct igb_q_vector *q_vector, struct sk_buff *skb) { }
int igb_ptp_rx_pktstamp(struct igb_q_vector *q_vector, void *va, ktime_t *timestamp) { return 0; }
int igb_ptp_hwtstamp_get(struct net_device *netdev, struct kernel_hwtstamp_config *config) { return -EOPNOTSUPP; }
int igb_ptp_hwtstamp_set(struct net_device *netdev, struct kernel_hwtstamp_config *config, struct netlink_ext_ack *extack) { return -EOPNOTSUPP; }

/* Sin AF_XDP zero-copy (igb_xsk.c): ningun pool XSK se asocia a una cola. */
struct xsk_buff_pool *igb_xsk_pool(struct igb_adapter *adapter, struct igb_ring *ring) { return NULL; }
int igb_xsk_pool_setup(struct igb_adapter *adapter, struct xsk_buff_pool *pool, u16 qid) { return -EOPNOTSUPP; }
bool igb_alloc_rx_buffers_zc(struct igb_ring *rx_ring, struct xsk_buff_pool *xsk_pool, u16 count) { return true; }
void igb_clean_rx_ring_zc(struct igb_ring *rx_ring) { }
int igb_clean_rx_irq_zc(struct igb_q_vector *q_vector, struct xsk_buff_pool *xsk_pool, const int budget) { return 0; }
bool igb_xmit_zc(struct igb_ring *tx_ring, struct xsk_buff_pool *xsk_pool) { return true; }
int igb_xsk_wakeup(struct net_device *dev, u32 qid, u32 flags) { return -EOPNOTSUPP; }
