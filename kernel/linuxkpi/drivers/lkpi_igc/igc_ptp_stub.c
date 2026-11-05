// SPDX-License-Identifier: GPL-2.0-only
/* PTP de igc: la capa no tiene reloj PTP (Plan_Red.md); sin marcas de tiempo
 * por hardware. El resto del driver no depende de esto. */
#include "igc.h"

void igc_ptp_init(struct igc_adapter *adapter) { }
void igc_ptp_reset(struct igc_adapter *adapter) { }
void igc_ptp_suspend(struct igc_adapter *adapter) { }
void igc_ptp_stop(struct igc_adapter *adapter) { }
ktime_t igc_ptp_rx_pktstamp(struct igc_adapter *adapter, __le32 *buf) { return 0; }
int igc_ptp_hwtstamp_get(struct net_device *netdev, struct kernel_hwtstamp_config *config) { return -EOPNOTSUPP; }
int igc_ptp_hwtstamp_set(struct net_device *netdev, struct kernel_hwtstamp_config *config, struct netlink_ext_ack *extack) { return -EOPNOTSUPP; }
void igc_ptp_tx_hang(struct igc_adapter *adapter) { }
void igc_ptp_clear_xsk_tx_tstamp_queue(struct igc_adapter *adapter, u16 queue_id) { }
void igc_ptp_read(struct igc_adapter *adapter, struct timespec64 *ts) { ts->tv_sec = 0; ts->tv_nsec = 0; }
void igc_ptp_tx_tstamp_event(struct igc_adapter *adapter) { }
