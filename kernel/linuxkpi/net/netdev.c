// SPDX-License-Identifier: GPL-2.0-only
/*
 * LinuxKPI-NET (Plan_Red.md): net_device, sk_buff, NAPI y colas de TX de
 * Linux sobre la pila de Aurora. Un driver de Linux ve un net_device normal;
 * al registrarlo aparece como ethN en netif_t (kernel/net/netif_lkpi.c).
 *
 *  - RX: napi_gro_receive()/netif_receive_skb() copian la trama a un skb_t de
 *    Aurora (sin GRO). eth_type_trans() quita la cabecera Ethernet y aqui se
 *    vuelve a poner.
 *  - TX: Aurora llama a lkpi_xmit() con la trama; se copia a un sk_buff
 *    lineal y se pasa a ndo_start_xmit(). Si la cola esta parada se descarta
 *    (TCP retransmite).
 *  - NAPI: un tasklet de la capa (contexto "softirq", expulsion desactivada).
 */
#include <linux/if_arp.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "../lkpi_internal.h"
#include "../aurora.h"

/* ------------------------------------------------------------------ skb */

struct sk_buff *__alloc_skb(unsigned int size, gfp_t gfp, int flags, int node)
{
	struct sk_buff *skb;
	unsigned int dsz = SKB_DATA_ALIGN(size);
	u8 *data;

	skb = kzalloc(sizeof(*skb), gfp);
	if (!skb)
		return NULL;
	data = kzalloc(dsz + SKB_DATA_ALIGN(sizeof(struct skb_shared_info)), gfp);
	if (!data) {
		kfree(skb);
		return NULL;
	}
	skb->head = data;
	skb->data = data;
	skb->tail = 0;
	skb->end = dsz;
	skb->truesize = dsz + sizeof(*skb);
	refcount_set(&skb->users, 1);
	skb->mac_header = skb->network_header = skb->transport_header = (typeof(skb->mac_header))~0U;
	return skb;
}
EXPORT_SYMBOL(__alloc_skb);

void kfree_skb(struct sk_buff *skb)
{
	if (!skb)
		return;
	if (!refcount_dec_and_test(&skb->users))
		return;
	kfree(skb->head);
	kfree(skb);
}
EXPORT_SYMBOL(kfree_skb);

struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int length, gfp_t gfp)
{
	struct sk_buff *skb = __alloc_skb(length + NET_SKB_PAD, gfp, 0, -1);
	if (skb) {
		skb_reserve(skb, NET_SKB_PAD);
		skb->dev = dev;
	}
	return skb;
}
EXPORT_SYMBOL(__netdev_alloc_skb);

struct sk_buff *napi_alloc_skb(struct napi_struct *napi, unsigned int length)
{
	return __netdev_alloc_skb(napi->dev, length, GFP_ATOMIC);
}
EXPORT_SYMBOL(napi_alloc_skb);

void *skb_header_pointer_slow(const struct sk_buff *skb, int offset, int len, void *buffer)
{
	if (offset < 0 || offset + len > (int)skb->len)
		return NULL;
	memcpy(buffer, skb->data + offset, len);	/* lineal: no se llega */
	return buffer;
}

int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int len)
{
	if (offset < 0 || offset + len > (int)skb->len)
		return -EFAULT;
	memcpy(to, skb->data + offset, len);
	return 0;
}

int skb_linearize(struct sk_buff *skb) { return 0; }

int pskb_expand_head(struct sk_buff *skb, int nhead, int ntail, gfp_t gfp)
{
	unsigned int size = (skb->end + nhead + ntail);
	unsigned int dsz = SKB_DATA_ALIGN(size);
	u8 *nd = kzalloc(dsz + SKB_DATA_ALIGN(sizeof(struct skb_shared_info)), gfp);
	long off = skb->data - skb->head;

	if (!nd)
		return -ENOMEM;
	memcpy(nd + nhead, skb->head, skb->end);
	kfree(skb->head);
	skb->head = nd;
	skb->data = nd + off + nhead;
	skb->end = dsz;
	if (skb->mac_header != (typeof(skb->mac_header))~0U) skb->mac_header += nhead;
	if (skb->network_header != (typeof(skb->network_header))~0U) skb->network_header += nhead;
	if (skb->transport_header != (typeof(skb->transport_header))~0U) skb->transport_header += nhead;
	skb->cloned = 0;
	return 0;
}

int skb_checksum_help(struct sk_buff *skb)
{
	/* checksum por software de un CHECKSUM_PARTIAL (Aurora no los genera) */
	u32 sum = 0;
	unsigned int i, start = skb_checksum_start_offset(skb);
	u8 *p = skb->data + start;
	unsigned int len = skb->len - start;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;
	for (i = 0; i + 1 < len; i += 2)
		sum += (p[i] << 8) | p[i + 1];
	if (len & 1)
		sum += p[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	sum = ~sum & 0xffff;
	p[skb->csum_offset] = sum >> 8;
	p[skb->csum_offset + 1] = sum;
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

/* ------------------------------------------------------------- ethernet */

__be16 eth_type_trans(struct sk_buff *skb, struct net_device *dev)
{
	struct ethhdr *eth;

	skb->dev = dev;
	skb_reset_mac_header(skb);
	eth = (struct ethhdr *)skb->data;
	skb_pull(skb, ETH_HLEN);
	if (is_multicast_ether_addr(eth->h_dest))
		skb->pkt_type = ether_addr_equal(eth->h_dest, dev->broadcast) ?
				PACKET_BROADCAST : PACKET_MULTICAST;
	else if (!ether_addr_equal(eth->h_dest, dev->dev_addr))
		skb->pkt_type = PACKET_OTHERHOST;
	else
		skb->pkt_type = PACKET_HOST;
	if (eth_proto_is_802_3(eth->h_proto))
		return eth->h_proto;
	return htons(ETH_P_802_2);
}
EXPORT_SYMBOL(eth_type_trans);

int eth_validate_addr(struct net_device *dev)
{
	return is_valid_ether_addr(dev->dev_addr) ? 0 : -EADDRNOTAVAIL;
}
EXPORT_SYMBOL(eth_validate_addr);

int eth_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr((u8 *)addr->sa_data))
		return -EADDRNOTAVAIL;
	eth_hw_addr_set(dev, (u8 *)addr->sa_data);
	return 0;
}
EXPORT_SYMBOL(eth_mac_addr);

int eth_platform_get_mac_address(struct device *dev, u8 *mac_addr) { return -ENODEV; }

u32 ethtool_op_get_link(struct net_device *dev) { return netif_carrier_ok(dev) ? 1 : 0; }
EXPORT_SYMBOL(ethtool_op_get_link);

int ethtool_op_get_ts_info(struct net_device *dev, struct kernel_ethtool_ts_info *info)
{
	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
				SOF_TIMESTAMPING_SOFTWARE;
	info->phc_index = -1;
	return 0;
}
EXPORT_SYMBOL(ethtool_op_get_ts_info);

/* ----------------------------------------------------------- net_device */

static DEFINE_MUTEX(lkpi_rtnl);
void rtnl_lock(void) { mutex_lock(&lkpi_rtnl); }
void rtnl_unlock(void) { mutex_unlock(&lkpi_rtnl); }
int rtnl_is_locked(void) { return mutex_is_locked(&lkpi_rtnl); }
EXPORT_SYMBOL(rtnl_lock);
EXPORT_SYMBOL(rtnl_unlock);

const char *netdev_name(const struct net_device *dev) { return dev->name; }

struct lkpi_ndev_extra {
	struct ethtool_netdev_state ethtool;
	spinlock_t xmit_lock;
	void *handle;		/* aurora_netif_register() */
	bool wifi_link;		/* Wi-Fi: la pila de Aurora ve enlace solo tras el 4-way handshake */
};

struct net_device *lkpi_alloc_netdev_mqs(int sizeof_priv, const char *name,
					 unsigned int txqs, unsigned int rxqs)
{
	size_t base = ALIGN(sizeof(struct net_device), NETDEV_ALIGN);
	size_t extra = ALIGN(sizeof(struct lkpi_ndev_extra), 32);
	struct net_device *dev = kzalloc(base + ALIGN(sizeof_priv, 32) + extra, GFP_KERNEL);
	struct lkpi_ndev_extra *x;

	if (!dev)
		return NULL;
	if (!txqs) txqs = 1;
	if (!rxqs) rxqs = 1;
	dev->_tx = kcalloc(txqs, sizeof(*dev->_tx), GFP_KERNEL);
	if (!dev->_tx) {
		kfree(dev);
		return NULL;
	}
	dev->num_tx_queues = dev->real_num_tx_queues = txqs;
	dev->num_rx_queues = dev->real_num_rx_queues = rxqs;
	x = (struct lkpi_ndev_extra *)((char *)dev + base + ALIGN(sizeof_priv, 32));
	dev->lkpi_priv_area = x;
	dev->ethtool = &x->ethtool;
	spin_lock_init(&x->xmit_lock);
	INIT_LIST_HEAD(&dev->mc.list);
	dev->dev_addr = dev->dev_addr_buf;
	dev->addr_len = ETH_ALEN;
	dev->mtu = ETH_DATA_LEN;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_DATA_LEN;
	dev->gso_max_size = 65536;
	dev->gso_max_segs = 65535;
	dev->tso_max_size = 65536;
	dev->tso_max_segs = 65535;
	dev->type = 1;
	dev->flags = IFF_BROADCAST | IFF_MULTICAST;
	memset(dev->broadcast, 0xff, ETH_ALEN);
	set_bit(__LINK_STATE_PRESENT, &dev->state);
	set_bit(__LINK_STATE_NOCARRIER, &dev->state);
	device_initialize(&dev->dev);
	strscpy(dev->name, name ? name : "eth%d", sizeof(dev->name));
	return dev;
}
EXPORT_SYMBOL(lkpi_alloc_netdev_mqs);

struct net_device *alloc_etherdev_mqs(int sizeof_priv, unsigned int txqs, unsigned int rxqs)
{
	return lkpi_alloc_netdev_mqs(sizeof_priv, "eth%d", txqs, rxqs);
}
EXPORT_SYMBOL(alloc_etherdev_mqs);

void free_netdev(struct net_device *dev)
{
	if (!dev)
		return;
	kfree(dev->_tx);
	kfree(dev);
}
EXPORT_SYMBOL(free_netdev);

static void lkpi_devm_free_netdev(struct device *d, void *res)
{
	struct net_device **p = res;
	free_netdev(*p);
}

struct net_device *devm_alloc_etherdev_mqs(struct device *dev, int sizeof_priv,
					   unsigned int txqs, unsigned int rxqs)
{
	struct net_device *nd = alloc_etherdev_mqs(sizeof_priv, txqs, rxqs);
	struct net_device **p;

	if (!nd)
		return NULL;
	p = devres_alloc(lkpi_devm_free_netdev, sizeof(*p), GFP_KERNEL);
	if (!p) {
		free_netdev(nd);
		return NULL;
	}
	*p = nd;
	devres_add(dev, p);
	return nd;
}
EXPORT_SYMBOL(devm_alloc_etherdev_mqs);

static void lkpi_devm_unreg_netdev(struct device *d, void *res)
{
	struct net_device **p = res;
	unregister_netdev(*p);
}

int devm_register_netdev(struct device *dev, struct net_device *ndev)
{
	struct net_device **p;
	int ret = register_netdev(ndev);

	if (ret)
		return ret;
	p = devres_alloc(lkpi_devm_unreg_netdev, sizeof(*p), GFP_KERNEL);
	if (!p) {
		unregister_netdev(ndev);
		return -ENOMEM;
	}
	*p = ndev;
	devres_add(dev, p);
	return 0;
}
EXPORT_SYMBOL(devm_register_netdev);

#define NDX(dev) ((struct lkpi_ndev_extra *)(dev)->lkpi_priv_area)

/* NAPI -------------------------------------------------------------- */

#define NAPI_SCHED	0
#define NAPI_DISABLED	1

static void lkpi_napi_run(unsigned long data)
{
	struct napi_struct *n = (struct napi_struct *)data;
	int work;

	if (test_bit(NAPI_DISABLED, &n->state)) {
		/* napi_disable() espera a que SCHED se limpie */
		clear_bit(NAPI_SCHED, &n->state);
		n->lkpi_scheduled = 0;
		return;
	}
	work = n->poll(n, n->weight);
	if (work >= n->weight && test_bit(NAPI_SCHED, &n->state))
		tasklet_schedule(&n->lkpi_tl);	/* sigue pendiente */
}

void netif_napi_add_weight(struct net_device *dev, struct napi_struct *napi,
			   int (*poll)(struct napi_struct *, int), int weight)
{
	napi->dev = dev;
	napi->poll = poll;
	napi->weight = weight;
	napi->state = 1UL << NAPI_DISABLED;
	napi->lkpi_scheduled = 0;
	tasklet_init(&napi->lkpi_tl, lkpi_napi_run, (unsigned long)napi);
}
EXPORT_SYMBOL(netif_napi_add_weight);

void netif_napi_del(struct napi_struct *napi) { tasklet_kill(&napi->lkpi_tl); }
EXPORT_SYMBOL(netif_napi_del);

void napi_enable(struct napi_struct *n)
{
	clear_bit(NAPI_DISABLED, &n->state);
	clear_bit(NAPI_SCHED, &n->state);
}
EXPORT_SYMBOL(napi_enable);

void napi_disable(struct napi_struct *n)
{
	set_bit(NAPI_DISABLED, &n->state);
	while (test_bit(NAPI_SCHED, &n->state))
		msleep(1);
	tasklet_kill(&n->lkpi_tl);
}
EXPORT_SYMBOL(napi_disable);

bool napi_schedule_prep(struct napi_struct *n)
{
	if (test_bit(NAPI_DISABLED, &n->state))
		return false;
	return !test_and_set_bit(NAPI_SCHED, &n->state);
}
EXPORT_SYMBOL(napi_schedule_prep);

void __napi_schedule(struct napi_struct *n)
{
	n->lkpi_scheduled = 1;
	tasklet_schedule(&n->lkpi_tl);
}
EXPORT_SYMBOL(__napi_schedule);

bool napi_complete_done(struct napi_struct *n, int work_done)
{
	if (!test_bit(NAPI_SCHED, &n->state))
		return false;
	n->lkpi_scheduled = 0;
	smp_mb__before_atomic();
	clear_bit(NAPI_SCHED, &n->state);
	return true;
}
EXPORT_SYMBOL(napi_complete_done);

/* RX ---------------------------------------------------------------- */

/* Wi-Fi: el gestor de aurora_wifi (lkpi_cfg80211) se queda con las tramas EAPOL. */
void (*lkpi_eapol_rx_hook)(struct net_device *dev, struct sk_buff *skb);
EXPORT_SYMBOL(lkpi_eapol_rx_hook);

static void lkpi_deliver(struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;
	struct lkpi_ndev_extra *x = dev ? NDX(dev) : NULL;

	if (dev && dev->ieee80211_ptr && skb->protocol == htons(ETH_P_PAE) &&
	    lkpi_eapol_rx_hook) {
		lkpi_eapol_rx_hook(dev, skb);	/* toma posesion del skb */
		return;
	}

	if (x && x->handle && netif_running(dev)) {
		/* eth_type_trans() quito la cabecera Ethernet: se repone */
		skb_push(skb, ETH_HLEN);
		if (!aurora_netif_rx(x->handle, skb->data, skb->len)) {
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += skb->len;
		} else {
			dev->stats.rx_dropped++;
		}
	}
	kfree_skb(skb);
}

gro_result_t napi_gro_receive(struct napi_struct *napi, struct sk_buff *skb)
{
	lkpi_deliver(skb);
	return GRO_NORMAL;
}
EXPORT_SYMBOL(napi_gro_receive);

int netif_receive_skb(struct sk_buff *skb) { lkpi_deliver(skb); return 0; }
int netif_rx(struct sk_buff *skb) { lkpi_deliver(skb); return 0; }
EXPORT_SYMBOL(netif_receive_skb);
EXPORT_SYMBOL(netif_rx);

/* TX ---------------------------------------------------------------- */

static int lkpi_xmit(void *cookie, const void *frame, size_t len)
{
	struct net_device *dev = cookie;
	struct sk_buff *skb;
	unsigned long flags;
	netdev_tx_t rc;

	if (!netif_running(dev) || !netif_carrier_ok(dev))
		return -ENETDOWN;
	if (netif_queue_stopped(dev))
		return -EAGAIN;
	skb = alloc_skb(len + NET_SKB_PAD, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, NET_SKB_PAD);
	skb_put_data(skb, frame, len);
	skb->dev = dev;
	skb->protocol = ((const u8 *)frame)[12] << 8 | ((const u8 *)frame)[13];
	skb->protocol = htons(skb->protocol);
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, ETH_HLEN);
	skb->ip_summed = CHECKSUM_NONE;

	spin_lock_irqsave(&NDX(dev)->xmit_lock, flags);
	rc = dev->netdev_ops->ndo_start_xmit(skb, dev);
	spin_unlock_irqrestore(&NDX(dev)->xmit_lock, flags);
	if (rc == NETDEV_TX_OK) {
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += len;
		return 0;
	}
	kfree_skb(skb);
	return -EAGAIN;
}

void netif_tx_wake_queue(struct netdev_queue *q) { clear_bit(__QUEUE_STATE_DRV_XOFF, &q->state); }
EXPORT_SYMBOL(netif_tx_wake_queue);
int netif_set_real_num_tx_queues(struct net_device *dev, unsigned int txq)
{ dev->real_num_tx_queues = txq; return 0; }
int netif_set_real_num_rx_queues(struct net_device *dev, unsigned int rxq)
{ dev->real_num_rx_queues = rxq; return 0; }

/* estado / registro --------------------------------------------------- */

/* Wi-Fi: enlace hacia la pila de Aurora (DHCP) solo cuando aurora_wifi lo pide. */
void lkpi_netif_wifi_link(struct net_device *dev, bool up)
{
	struct lkpi_ndev_extra *x = NDX(dev);

	x->wifi_link = up;
	if (x->handle)
		aurora_netif_carrier(x->handle, up ? 1 : 0);
}
EXPORT_SYMBOL(lkpi_netif_wifi_link);

void netif_carrier_on(struct net_device *dev)
{
	if (test_and_clear_bit(__LINK_STATE_NOCARRIER, &dev->state) && NDX(dev)->handle &&
	    !dev->ieee80211_ptr)
		aurora_netif_carrier(NDX(dev)->handle, 1);
}
EXPORT_SYMBOL(netif_carrier_on);

void netif_carrier_off(struct net_device *dev)
{
	if (!test_and_set_bit(__LINK_STATE_NOCARRIER, &dev->state) && NDX(dev)->handle) {
		NDX(dev)->wifi_link = false;
		aurora_netif_carrier(NDX(dev)->handle, 0);
	}
}
EXPORT_SYMBOL(netif_carrier_off);

void netif_device_detach(struct net_device *dev)
{
	clear_bit(__LINK_STATE_PRESENT, &dev->state);
	netif_tx_stop_all_queues(dev);
}
EXPORT_SYMBOL(netif_device_detach);

void netif_device_attach(struct net_device *dev)
{
	if (!test_and_set_bit(__LINK_STATE_PRESENT, &dev->state) && netif_running(dev))
		netif_tx_wake_all_queues(dev);
}
EXPORT_SYMBOL(netif_device_attach);

void netdev_update_features(struct net_device *dev)
{
	netdev_features_t f = dev->wanted_features ? dev->wanted_features : dev->features;

	if (dev->netdev_ops->ndo_fix_features)
		f = dev->netdev_ops->ndo_fix_features(dev, f);
	f &= dev->hw_features | NETIF_F_HIGHDMA;
	if (f != dev->features && dev->netdev_ops->ndo_set_features)
		dev->netdev_ops->ndo_set_features(dev, f);
	dev->features = f;
}
EXPORT_SYMBOL(netdev_update_features);

void netdev_stats_to_stats64(struct rtnl_link_stats64 *s, const struct net_device_stats *n)
{
	const unsigned long *src = (const unsigned long *)n;
	u64 *dst = (u64 *)s;
	size_t i, cnt = sizeof(*n) / sizeof(unsigned long);

	for (i = 0; i < cnt; i++)
		dst[i] = src[i];
}
EXPORT_SYMBOL(netdev_stats_to_stats64);

void dev_sw_netstats_rx_add(struct net_device *dev, unsigned int len)
{ dev->stats.rx_packets++; dev->stats.rx_bytes += len; }
void dev_sw_netstats_tx_add(struct net_device *dev, unsigned int packets, unsigned int len)
{ dev->stats.tx_packets += packets; dev->stats.tx_bytes += len; }
void dev_fetch_sw_netstats(struct rtnl_link_stats64 *s, const struct pcpu_sw_netstats __percpu *netstats) { }
EXPORT_SYMBOL(dev_sw_netstats_rx_add);
EXPORT_SYMBOL(dev_sw_netstats_tx_add);
EXPORT_SYMBOL(dev_fetch_sw_netstats);

int dev_open(struct net_device *dev, void *extack)
{
	int ret;

	if (netif_running(dev))
		return 0;
	if (dev->netdev_ops->ndo_validate_addr) {
		ret = dev->netdev_ops->ndo_validate_addr(dev);
		if (ret)
			return ret;
	}
	set_bit(__LINK_STATE_START, &dev->state);
	ret = dev->netdev_ops->ndo_open(dev);
	if (ret) {
		clear_bit(__LINK_STATE_START, &dev->state);
		return ret;
	}
	dev->flags |= IFF_UP;
	if (dev->netdev_ops->ndo_set_rx_mode)
		dev->netdev_ops->ndo_set_rx_mode(dev);
	return 0;
}
EXPORT_SYMBOL(dev_open);

void dev_close(struct net_device *dev)
{
	if (!netif_running(dev))
		return;
	netif_tx_stop_all_queues(dev);
	dev->netdev_ops->ndo_stop(dev);
	clear_bit(__LINK_STATE_START, &dev->state);
	dev->flags &= ~IFF_UP;
}
EXPORT_SYMBOL(dev_close);

static int lkpi_aurora_open(void *cookie)
{
	struct net_device *dev = cookie;
	return dev_open(dev, NULL);
}

static int lkpi_aurora_stop(void *cookie)
{
	dev_close(cookie);
	return 0;
}

static const struct aurora_netif_ops lkpi_nic_ops = {
	.open = lkpi_aurora_open,
	.stop = lkpi_aurora_stop,
	.xmit = lkpi_xmit,
};

int register_netdev(struct net_device *dev)
{
	int ret;
	char nm[8];
	void *h;

	if (dev->netdev_ops->ndo_init) {
		ret = dev->netdev_ops->ndo_init(dev);
		if (ret)
			return ret;
	}
	if (!is_valid_ether_addr(dev->dev_addr)) {
		eth_hw_addr_random(dev);
		dev_warn(&dev->dev, "MAC invalida, usando una aleatoria\n");
	}
	if (is_zero_ether_addr(dev->perm_addr))
		memcpy(dev->perm_addr, dev->dev_addr, ETH_ALEN);
	if (dev->netdev_ops->ndo_set_features || dev->hw_features)
		netdev_update_features(dev);
	h = aurora_netif_register(dev->dev_addr, dev, &lkpi_nic_ops, nm);
	if (!h)
		return -ENOMEM;
	NDX(dev)->handle = h;
	strscpy(dev->name, nm, sizeof(dev->name));
	dev->lkpi_registered = 1;
	pr_info("%s: registrada (%pM)\n", dev->name, dev->dev_addr);
	/* como un "ip link set up": el sistema no tiene aun red de usuario */
	ret = dev_open(dev, NULL);
	if (ret)
		pr_err("%s: no abre (%d)\n", dev->name, ret);
	return 0;
}
EXPORT_SYMBOL(register_netdev);

void unregister_netdev(struct net_device *dev)
{
	if (!dev->lkpi_registered)
		return;
	dev_close(dev);
	aurora_netif_unregister(NDX(dev)->handle);
	NDX(dev)->handle = NULL;
	if (dev->netdev_ops->ndo_uninit)
		dev->netdev_ops->ndo_uninit(dev);
	dev->lkpi_registered = 0;
}
EXPORT_SYMBOL(unregister_netdev);

/* PCI: errores de estado (r8169) ---------------------------------------- */

u16 pci_status_get_and_clear_errors(struct pci_dev *pdev)
{
	u16 status;
	const u16 mask = 0xf900;	/* PARITY | SIG_TARGET_ABORT | REC_* | SIG_SYSTEM_ERROR | DETECTED_PARITY */

	if (pci_read_config_word(pdev, PCI_STATUS, &status))
		return 0;
	status &= mask;
	if (status)
		pci_write_config_word(pdev, PCI_STATUS, status);
	return status;
}
EXPORT_SYMBOL(pci_status_get_and_clear_errors);

/* ethtool: conversion de los modos de enlace legacy (net/ethtool/ioctl.c) */
void ethtool_convert_legacy_u32_to_link_mode(unsigned long *dst, u32 legacy_u32)
{
	bitmap_zero(dst, __ETHTOOL_LINK_MODE_MASK_NBITS);
	dst[0] = legacy_u32;
}
EXPORT_SYMBOL(ethtool_convert_legacy_u32_to_link_mode);

bool ethtool_convert_link_mode_to_legacy_u32(u32 *legacy_u32, const unsigned long *src)
{
	bool retval = true;

	/* alguno de los bits 32.. activo: no cabe en la forma legacy */
	if (find_next_bit(src, __ETHTOOL_LINK_MODE_MASK_NBITS, 32) < __ETHTOOL_LINK_MODE_MASK_NBITS)
		retval = false;
	*legacy_u32 = src[0];
	return retval;
}
EXPORT_SYMBOL(ethtool_convert_link_mode_to_legacy_u32);

EXPORT_SYMBOL(eth_platform_get_mac_address);
EXPORT_SYMBOL(netdev_name);
EXPORT_SYMBOL(netif_set_real_num_rx_queues);
EXPORT_SYMBOL(netif_set_real_num_tx_queues);
EXPORT_SYMBOL(pskb_expand_head);
EXPORT_SYMBOL(rtnl_is_locked);
EXPORT_SYMBOL(skb_checksum_help);
EXPORT_SYMBOL(skb_copy_bits);
EXPORT_SYMBOL(skb_header_pointer_slow);
EXPORT_SYMBOL(skb_linearize);

/* Cabecera "util" de una trama: sin analizador de flujos, la cabecera Ethernet
 * mas lo que quepa de IP/TCP razonablemente (igc copia esto al area lineal). */
unsigned int eth_get_headlen(const struct net_device *dev, const void *data, unsigned int len)
{
	return len < 128 ? len : 128;
}
EXPORT_SYMBOL(eth_get_headlen);

/* ---- ethtool: cadenas de estadisticas ---- */
void ethtool_puts(u8 **data, const char *str)
{
	strscpy((char *)*data, str, ETH_GSTRING_LEN);
	*data += ETH_GSTRING_LEN;
}
EXPORT_SYMBOL(ethtool_puts);

void ethtool_sprintf(u8 **data, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf((char *)*data, ETH_GSTRING_LEN, fmt, args);
	va_end(args);
	*data += ETH_GSTRING_LEN;
}
EXPORT_SYMBOL(ethtool_sprintf);

/* ---- MAC Merge (frame preemption, 802.3br): no soportado en la capa.
 * El driver se registra pero el MM queda desactivado. ---- */
void ethtool_mmsv_init(struct ethtool_mmsv *mmsv, struct net_device *dev, const struct ethtool_mmsv_ops *ops)
{
	memset(mmsv, 0, sizeof(*mmsv));
	mmsv->ops = ops;
	mmsv->dev = dev;
	spin_lock_init(&mmsv->lock);
}
EXPORT_SYMBOL(ethtool_mmsv_init);
void ethtool_mmsv_stop(struct ethtool_mmsv *mmsv) { }
EXPORT_SYMBOL(ethtool_mmsv_stop);
void ethtool_mmsv_link_state_handle(struct ethtool_mmsv *mmsv, bool up) { }
EXPORT_SYMBOL(ethtool_mmsv_link_state_handle);
void ethtool_mmsv_event_handle(struct ethtool_mmsv *mmsv, enum ethtool_mmsv_event event) { }
EXPORT_SYMBOL(ethtool_mmsv_event_handle);
void ethtool_mmsv_get_mm(struct ethtool_mmsv *mmsv, struct ethtool_mm_state *state)
{
	memset(state, 0, sizeof(*state));
}
EXPORT_SYMBOL(ethtool_mmsv_get_mm);
void ethtool_mmsv_set_mm(struct ethtool_mmsv *mmsv, struct ethtool_mm_cfg *cfg) { }
EXPORT_SYMBOL(ethtool_mmsv_set_mm);

/* ---- un solo espacio de nombres, indices de dispositivo y notificadores ---- */
struct net init_net;
EXPORT_SYMBOL(init_net);

static DEFINE_MUTEX(lkpi_notif_lock);
static LIST_HEAD(lkpi_notifiers);
struct lkpi_notif { struct list_head node; struct notifier_block *nb; };
static struct net_device *lkpi_devs[32];
static int lkpi_next_ifindex = 1;

int call_netdevice_notifiers(unsigned long val, struct net_device *dev)
{
	struct lkpi_notif *n;
	struct netdev_notifier_info info = { .dev = dev };
	int ret = NOTIFY_DONE;

	mutex_lock(&lkpi_notif_lock);
	list_for_each_entry(n, &lkpi_notifiers, node) {
		ret = n->nb->notifier_call(n->nb, val, &info);
		if (ret & NOTIFY_STOP_MASK)
			break;
	}
	mutex_unlock(&lkpi_notif_lock);
	return ret;
}
EXPORT_SYMBOL(call_netdevice_notifiers);

int register_netdevice_notifier(struct notifier_block *nb)
{
	struct lkpi_notif *n = kzalloc(sizeof(*n), GFP_KERNEL);
	int i;

	if (!n)
		return -ENOMEM;
	n->nb = nb;
	mutex_lock(&lkpi_notif_lock);
	list_add_tail(&n->node, &lkpi_notifiers);
	/* como Linux: se le notifican los dispositivos que ya existen */
	for (i = 0; i < ARRAY_SIZE(lkpi_devs); i++) {
		struct netdev_notifier_info info = { .dev = lkpi_devs[i] };
		if (!lkpi_devs[i])
			continue;
		nb->notifier_call(nb, NETDEV_REGISTER, &info);
		if (netif_running(lkpi_devs[i]))
			nb->notifier_call(nb, NETDEV_UP, &info);
	}
	mutex_unlock(&lkpi_notif_lock);
	return 0;
}
EXPORT_SYMBOL(register_netdevice_notifier);

int unregister_netdevice_notifier(struct notifier_block *nb)
{
	struct lkpi_notif *n, *t;

	mutex_lock(&lkpi_notif_lock);
	list_for_each_entry_safe(n, t, &lkpi_notifiers, node) {
		if (n->nb != nb)
			continue;
		list_del(&n->node);
		kfree(n);
	}
	mutex_unlock(&lkpi_notif_lock);
	return 0;
}
EXPORT_SYMBOL(unregister_netdevice_notifier);

static void lkpi_dev_index_add(struct net_device *dev)
{
	int i;

	dev->ifindex = lkpi_next_ifindex++;
	for (i = 0; i < ARRAY_SIZE(lkpi_devs); i++)
		if (!lkpi_devs[i]) {
			lkpi_devs[i] = dev;
			return;
		}
}

static void lkpi_dev_index_del(struct net_device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lkpi_devs); i++)
		if (lkpi_devs[i] == dev)
			lkpi_devs[i] = NULL;
}

struct net_device *__dev_get_by_index(struct net *net, int ifindex)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lkpi_devs); i++)
		if (lkpi_devs[i] && lkpi_devs[i]->ifindex == ifindex)
			return lkpi_devs[i];
	return NULL;
}
EXPORT_SYMBOL(__dev_get_by_index);

struct net_device *dev_get_by_index(struct net *net, int ifindex)
{
	return __dev_get_by_index(net, ifindex);
}
EXPORT_SYMBOL(dev_get_by_index);

/* register_netdevice: alta "de dentro" (mac80211/cfg80211 llaman con el rtnl). No
 * crea la interfaz de Aurora: eso lo hace quien levante el dispositivo. */
int register_netdevice(struct net_device *dev)
{
	int ret;

	if (dev->netdev_ops && dev->netdev_ops->ndo_init) {
		ret = dev->netdev_ops->ndo_init(dev);
		if (ret)
			return ret;
	}
	lkpi_dev_index_add(dev);
	call_netdevice_notifiers(NETDEV_POST_INIT, dev);
	call_netdevice_notifiers(NETDEV_REGISTER, dev);
	return 0;
}
EXPORT_SYMBOL(register_netdevice);

void unregister_netdevice(struct net_device *dev)
{
	if (netif_running(dev))
		dev_close(dev);
	call_netdevice_notifiers(NETDEV_UNREGISTER, dev);
	lkpi_dev_index_del(dev);
	if (dev->netdev_ops && dev->netdev_ops->ndo_uninit)
		dev->netdev_ops->ndo_uninit(dev);
}
EXPORT_SYMBOL(unregister_netdevice);

/* pernet: un solo espacio, init() al registrar y exit() al retirar */
int register_pernet_device(struct pernet_operations *ops)
{
	return ops->init ? ops->init(&init_net) : 0;
}
EXPORT_SYMBOL(register_pernet_device);
void unregister_pernet_device(struct pernet_operations *ops)
{
	if (ops->pre_exit)
		ops->pre_exit(&init_net);
	if (ops->exit)
		ops->exit(&init_net);
}
EXPORT_SYMBOL(unregister_pernet_device);
int register_pernet_subsys(struct pernet_operations *ops) { return register_pernet_device(ops); }
EXPORT_SYMBOL(register_pernet_subsys);
void unregister_pernet_subsys(struct pernet_operations *ops) { unregister_pernet_device(ops); }
EXPORT_SYMBOL(unregister_pernet_subsys);
const struct kobj_ns_type_operations net_ns_type_operations;
EXPORT_SYMBOL(net_ns_type_operations);

/* ---- copias de skb: sin datos compartidos (clone == copy) ----
 * La capa no comparte paquetes entre skbs; clonar copia el contenido. */
struct sk_buff *skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
	struct sk_buff *n = __alloc_skb(skb->end, gfp, 0, -1);
	unsigned char *head;
	refcount_t users;

	if (!n)
		return NULL;
	head = n->head;
	users = n->users;
	memcpy(n, skb, sizeof(*n));
	n->head = head;
	n->users = users;
	n->next = n->prev = NULL;
	memcpy(head, skb->head, skb->end);
	n->data = head + (skb->data - skb->head);
	return n;
}
EXPORT_SYMBOL(skb_copy);

struct sk_buff *skb_clone(struct sk_buff *skb, gfp_t gfp) { return skb_copy(skb, gfp); }
EXPORT_SYMBOL(skb_clone);
struct sk_buff *pskb_copy(struct sk_buff *skb, gfp_t gfp) { return skb_copy(skb, gfp); }
EXPORT_SYMBOL(pskb_copy);

struct sk_buff *skb_copy_expand(const struct sk_buff *skb, int newheadroom, int newtailroom, gfp_t gfp)
{
	struct sk_buff *n = alloc_skb(newheadroom + skb->len + newtailroom, gfp);

	if (!n)
		return NULL;
	skb_reserve(n, newheadroom);
	skb_put_data(n, skb->data, skb->len);
	n->dev = skb->dev;
	n->protocol = skb->protocol;
	n->priority = skb->priority;
	n->queue_mapping = skb->queue_mapping;
	memcpy(n->cb, skb->cb, sizeof(n->cb));
	return n;
}
EXPORT_SYMBOL(skb_copy_expand);

struct sk_buff *skb_realloc_headroom(struct sk_buff *skb, unsigned int headroom)
{
	if (skb_headroom(skb) >= headroom)
		return skb_copy(skb, GFP_ATOMIC);
	return skb_copy_expand(skb, headroom, skb_tailroom(skb), GFP_ATOMIC);
}
EXPORT_SYMBOL(skb_realloc_headroom);

void ether_setup(struct net_device *dev)
{
	dev->type = ARPHRD_ETHER;
	dev->hard_header_len = ETH_HLEN;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_DATA_LEN;
	dev->mtu = ETH_DATA_LEN;
	dev->addr_len = ETH_ALEN;
	dev->flags = IFF_BROADCAST | IFF_MULTICAST;
}
EXPORT_SYMBOL(ether_setup);

int dev_queue_xmit(struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;

	if (!dev || !dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit) {
		kfree_skb(skb);
		return -ENODEV;
	}
	return dev->netdev_ops->ndo_start_xmit(skb, dev) == NETDEV_TX_OK ? 0 : -EBUSY;
}
EXPORT_SYMBOL(dev_queue_xmit);

int dev_alloc_name(struct net_device *dev, const char *name)
{
	static int n;
	int len = strlen(name);

	if (len >= 2 && !strcmp(name + len - 2, "%d"))
		snprintf(dev->name, sizeof(dev->name), "%.*s%d", len - 2, name, n++);
	else
		strscpy(dev->name, name, sizeof(dev->name));
	return 0;
}
EXPORT_SYMBOL(dev_alloc_name);

void kfree_skb_list(struct sk_buff *segs)
{
	while (segs) {
		struct sk_buff *next = segs->next;

		kfree_skb(segs);
		segs = next;
	}
}
EXPORT_SYMBOL(kfree_skb_list);

void skb_complete_wifi_ack(struct sk_buff *skb, bool acked) { kfree_skb(skb); }
EXPORT_SYMBOL(skb_complete_wifi_ack);
