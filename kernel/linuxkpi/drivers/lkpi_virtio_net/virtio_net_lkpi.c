// SPDX-License-Identifier: GPL-2.0
/*
 * virtio-net para AuroraOS sobre LinuxKPI + lkpi_virtio (Plan_Red.md, N2).
 *
 * El drivers/net/virtio_net.c de Linux 6.18 arrastra XDP, AF_XDP, page_pool,
 * DIM, stats por cola, RSS y failover (mas de 7000 lineas y un centenar de
 * APIs que Aurora no tiene), asi que aqui va la parte que hace falta para
 * una NIC virtio normal, con el mismo ABI de dispositivo: un par de colas
 * rx/tx, MAC y estado de enlace desde la configuracion, buffers de 2 KiB.
 * Sin offloads (csum/GSO), sin buffers mergeables, sin cola de control.
 */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_net.h>

#define VNET_BUF_LEN 2048

struct vnet {
	struct virtio_device *vdev;
	struct net_device *dev;
	struct virtqueue *rvq, *svq;
	struct napi_struct napi;
	spinlock_t tx_lock;
	struct work_struct config_work;
	unsigned int hdr_len;
	bool has_status;
	char rname[32], sname[32];
};

static void vnet_update_link(struct vnet *vi)
{
	u16 status = VIRTIO_NET_S_LINK_UP;

	if (vi->has_status)
		virtio_cread(vi->vdev, struct virtio_net_config, status, &status);
	if (status & VIRTIO_NET_S_LINK_UP) {
		netif_carrier_on(vi->dev);
		netif_wake_queue(vi->dev);
	} else {
		netif_carrier_off(vi->dev);
		netif_stop_queue(vi->dev);
	}
}

static void vnet_config_work(struct work_struct *w)
{
	vnet_update_link(container_of(w, struct vnet, config_work));
}

static void vnet_config_changed(struct virtio_device *vdev)
{
	struct vnet *vi = vdev->priv;

	schedule_work(&vi->config_work);
}

/* ---- RX ---- */

static int vnet_add_rx_buf(struct vnet *vi, void *buf)
{
	struct scatterlist sg;

	sg_init_one(&sg, buf, VNET_BUF_LEN);
	return virtqueue_add_inbuf(vi->rvq, &sg, 1, buf, GFP_ATOMIC);
}

static void vnet_fill_rx(struct vnet *vi)
{
	for (;;) {
		void *buf = kmalloc(VNET_BUF_LEN, GFP_KERNEL);

		if (!buf)
			break;
		if (vnet_add_rx_buf(vi, buf)) {
			kfree(buf);
			break;
		}
	}
	virtqueue_kick(vi->rvq);
}

static void vnet_rx_done(struct virtqueue *vq)
{
	struct vnet *vi = vq->vdev->priv;

	if (napi_schedule_prep(&vi->napi)) {
		virtqueue_disable_cb(vq);
		__napi_schedule(&vi->napi);
	}
}

static int vnet_poll(struct napi_struct *napi, int budget)
{
	struct vnet *vi = container_of(napi, struct vnet, napi);
	struct net_device *dev = vi->dev;
	unsigned int len, done = 0;
	void *buf;

again:
	while (done < budget && (buf = virtqueue_get_buf(vi->rvq, &len))) {
		if (len >= vi->hdr_len + ETH_HLEN && len <= VNET_BUF_LEN) {
			unsigned int plen = len - vi->hdr_len;
			struct sk_buff *skb = napi_alloc_skb(napi, plen);

			if (skb) {
				skb_put_data(skb, buf + vi->hdr_len, plen);
				skb->protocol = eth_type_trans(skb, dev);
				dev->stats.rx_packets++;
				dev->stats.rx_bytes += plen;
				napi_gro_receive(napi, skb);
			} else {
				dev->stats.rx_dropped++;
			}
		} else {
			dev->stats.rx_length_errors++;
		}
		if (vnet_add_rx_buf(vi, buf))
			kfree(buf);	/* no cabe: el anillo se queda con un hueco menos */
		done++;
	}
	if (done)
		virtqueue_kick(vi->rvq);
	if (done < budget) {
		if (napi_complete_done(napi, done) && !virtqueue_enable_cb(vi->rvq)) {
			if (napi_schedule_prep(napi)) {
				virtqueue_disable_cb(vi->rvq);
				__napi_schedule(napi);
				goto again;
			}
		}
	}
	return done;
}

/* ---- TX ---- */

static void vnet_free_old_xmit(struct vnet *vi)
{
	unsigned int len;
	void *buf;

	while ((buf = virtqueue_get_buf(vi->svq, &len))) {
		vi->dev->stats.tx_packets++;
		kfree(buf);
	}
}

static void vnet_tx_done(struct virtqueue *vq)
{
	struct vnet *vi = vq->vdev->priv;
	unsigned long flags;

	spin_lock_irqsave(&vi->tx_lock, flags);
	vnet_free_old_xmit(vi);
	spin_unlock_irqrestore(&vi->tx_lock, flags);
	if (netif_queue_stopped(vi->dev) && netif_carrier_ok(vi->dev))
		netif_wake_queue(vi->dev);
}

static netdev_tx_t vnet_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct vnet *vi = netdev_priv(dev);
	struct scatterlist sg;
	unsigned long flags;
	unsigned int len = skb->len;
	u8 *buf;
	int err;

	if (len > VNET_BUF_LEN - vi->hdr_len) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	buf = kzalloc(vi->hdr_len + len, GFP_ATOMIC);
	if (!buf) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	skb_copy_bits(skb, 0, buf + vi->hdr_len, len);	/* cabecera virtio a cero: sin offloads */
	sg_init_one(&sg, buf, vi->hdr_len + len);

	spin_lock_irqsave(&vi->tx_lock, flags);
	vnet_free_old_xmit(vi);
	err = virtqueue_add_outbuf(vi->svq, &sg, 1, buf, GFP_ATOMIC);
	if (!err)
		dev->stats.tx_bytes += len;
	if (!err)
		virtqueue_kick(vi->svq);
	spin_unlock_irqrestore(&vi->tx_lock, flags);

	if (err) {
		kfree(buf);
		netif_stop_queue(dev);
		/* la finalizacion de TX la despertara; reintenta ya por si ya hay hueco */
		spin_lock_irqsave(&vi->tx_lock, flags);
		vnet_free_old_xmit(vi);
		spin_unlock_irqrestore(&vi->tx_lock, flags);
		netif_wake_queue(dev);
		return NETDEV_TX_BUSY;
	}
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---- netdev ---- */

static int vnet_open(struct net_device *dev)
{
	struct vnet *vi = netdev_priv(dev);

	napi_enable(&vi->napi);
	napi_schedule(&vi->napi);	/* recoge lo llegado mientras estaba parada */
	vnet_update_link(vi);
	return 0;
}

static int vnet_stop(struct net_device *dev)
{
	struct vnet *vi = netdev_priv(dev);

	netif_stop_queue(dev);
	napi_disable(&vi->napi);
	return 0;
}

static const struct net_device_ops vnet_netdev_ops = {
	.ndo_open = vnet_open,
	.ndo_stop = vnet_stop,
	.ndo_start_xmit = vnet_start_xmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_mac_address = eth_mac_addr,
};

static int vnet_probe(struct virtio_device *vdev)
{
	struct virtqueue *vqs[2];
	struct virtqueue_info info[2];
	struct net_device *dev;
	struct vnet *vi;
	u8 mac[ETH_ALEN];
	int err;

	dev = alloc_etherdev(sizeof(*vi));
	if (!dev)
		return -ENOMEM;
	SET_NETDEV_DEV(dev, &vdev->dev);
	vi = netdev_priv(dev);
	vi->vdev = vdev;
	vi->dev = dev;
	vdev->priv = vi;
	spin_lock_init(&vi->tx_lock);
	INIT_WORK(&vi->config_work, vnet_config_work);
	vi->hdr_len = virtio_has_feature(vdev, VIRTIO_F_VERSION_1) ||
		      virtio_has_feature(vdev, VIRTIO_NET_F_MRG_RXBUF) ?
		      sizeof(struct virtio_net_hdr_mrg_rxbuf) : sizeof(struct virtio_net_hdr);
	vi->has_status = virtio_has_feature(vdev, VIRTIO_NET_F_STATUS);

	dev->netdev_ops = &vnet_netdev_ops;
	dev->min_mtu = 68;
	dev->max_mtu = ETH_DATA_LEN;
	dev->mtu = ETH_DATA_LEN;
	if (virtio_has_feature(vdev, VIRTIO_NET_F_MAC)) {
		virtio_cread_bytes(vdev, offsetof(struct virtio_net_config, mac), mac, ETH_ALEN);
		eth_hw_addr_set(dev, mac);
	} else {
		eth_hw_addr_random(dev);
	}

	snprintf(vi->rname, sizeof(vi->rname), "input.0");
	snprintf(vi->sname, sizeof(vi->sname), "output.0");
	info[0] = (struct virtqueue_info){ .name = vi->rname, .callback = vnet_rx_done };
	info[1] = (struct virtqueue_info){ .name = vi->sname, .callback = vnet_tx_done };
	err = virtio_find_vqs(vdev, 2, vqs, info, NULL);
	if (err) {
		dev_err(&vdev->dev, "virtio_find_vqs: %d\n", err);
		goto free;
	}
	vi->rvq = vqs[0];
	vi->svq = vqs[1];
	netif_napi_add(dev, &vi->napi, vnet_poll);
	vnet_fill_rx(vi);

	virtio_device_ready(vdev);
	err = register_netdev(dev);
	if (err) {
		dev_err(&vdev->dev, "register_netdev: %d\n", err);
		goto del;
	}
	vnet_update_link(vi);
	dev_info(&vdev->dev, "virtio-net %pM\n", dev->dev_addr);
	return 0;
del:
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
free:
	free_netdev(dev);
	return err;
}

static void vnet_remove(struct virtio_device *vdev)
{
	struct vnet *vi = vdev->priv;
	void *buf;

	unregister_netdev(vi->dev);
	cancel_work_sync(&vi->config_work);
	virtio_reset_device(vdev);
	while ((buf = virtqueue_detach_unused_buf(vi->rvq)))
		kfree(buf);
	while ((buf = virtqueue_detach_unused_buf(vi->svq)))
		kfree(buf);
	netif_napi_del(&vi->napi);
	vdev->config->del_vqs(vdev);
	free_netdev(vi->dev);
}

static const struct virtio_device_id vnet_id_table[] = {
	{ VIRTIO_ID_NET, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static const unsigned int vnet_features[] = { VIRTIO_NET_F_MAC, VIRTIO_NET_F_STATUS };

static struct virtio_driver vnet_driver = {
	.feature_table = vnet_features,
	.feature_table_size = ARRAY_SIZE(vnet_features),
	.feature_table_legacy = vnet_features,
	.feature_table_size_legacy = ARRAY_SIZE(vnet_features),
	.driver.name = "virtio_net",
	.id_table = vnet_id_table,
	.probe = vnet_probe,
	.remove = vnet_remove,
	.config_changed = vnet_config_changed,
};
module_virtio_driver(vnet_driver);
MODULE_DEVICE_TABLE(virtio, vnet_id_table);
MODULE_DESCRIPTION("virtio-net compacto para AuroraOS (LinuxKPI)");
MODULE_LICENSE("GPL");
