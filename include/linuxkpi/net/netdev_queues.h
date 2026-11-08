#ifndef _LKPI_NET_NETDEV_QUEUES_H
#define _LKPI_NET_NETDEV_QUEUES_H
#include <linux/netdevice.h>
/* Versiones sin DQL de netif_txq_maybe_stop & co. get_desc devuelve los
 * descriptores libres. Devuelve 0 (ok), -1 (parada) o 1 (parada y reanudada). */
#define __netif_txq_maybe_stop(txq, get_desc, stop_thrs, start_thrs) ({ \
	int _res = 0; \
	if ((get_desc) < (stop_thrs)) { \
		netif_tx_stop_queue(txq); \
		smp_mb(); \
		if ((get_desc) >= (start_thrs)) { netif_tx_start_queue(txq); _res = 1; } else _res = -1; \
	} \
	_res; })
#define netif_txq_maybe_stop(txq, get_desc, stop_thrs, start_thrs) __netif_txq_maybe_stop(txq, get_desc, stop_thrs, start_thrs)
#define netif_subqueue_maybe_stop(dev, idx, get_desc, stop_thrs, start_thrs) \
	netif_txq_maybe_stop(netdev_get_tx_queue(dev, idx), get_desc, stop_thrs, start_thrs)
#define __netif_txq_completed_wake(txq, pkts, bytes, get_desc, start_thrs) ({ \
	int _res = 0; \
	if (netif_tx_queue_stopped(txq) && (get_desc) >= (start_thrs)) { netif_tx_wake_queue(txq); _res = 1; } \
	_res; })
#define netif_txq_completed_wake(txq, pkts, bytes, get_desc, start_thrs) __netif_txq_completed_wake(txq, pkts, bytes, get_desc, start_thrs)
#define netif_subqueue_completed_wake(dev, idx, pkts, bytes, get_desc, start_thrs) \
	netif_txq_completed_wake(netdev_get_tx_queue(dev, idx), pkts, bytes, get_desc, start_thrs)
#endif
