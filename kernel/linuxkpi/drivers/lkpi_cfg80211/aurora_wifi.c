// SPDX-License-Identifier: GPL-2.0
/*
 * Gestor Wi-Fi de AuroraOS (Plan_Red.md, N7.3).
 *
 * AuroraOS no tiene generic netlink, asi que nl80211 no existe y no se puede
 * usar un wpa_supplicant. Este fichero lo sustituye con lo imprescindible,
 * dentro de cfg80211 para poder usar sus funciones internas:
 *
 *   /dev/aurora-wifi   escritura: "scan\n", "connect\t<ssid>\t<clave>\n"
 *                                 (clave vacia = red abierta), "disconnect\n"
 *                      lectura:   estado + redes vistas (ver aw_snapshot)
 *
 * Soporta redes abiertas y WPA2-PSK (AKM 2, CCMP). El 4-way handshake y el de
 * clave de grupo se hacen aqui (wpa_psk.c, probado en el host); mac80211
 * instala las claves en el hardware. Sin WPA3/SAE, 802.1X, TKIP ni WEP.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/rtnetlink.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/cfg80211.h>
#include "core.h"
#include "rdev-ops.h"
#include "nl80211.h"
#include "wpa_psk.h"

#define ETH_P_PAE_ 0x888E

enum aw_state { AW_IDLE, AW_SCANNING, AW_CONNECTING, AW_HANDSHAKE, AW_CONNECTED, AW_FAILED };
static const char *const aw_state_name[] = {
	"idle", "scanning", "connecting", "handshake", "connected", "failed" };

static struct {
	struct mutex lock;			/* estado (nunca se toma wiphy/rtnl con ella; solo al reves) */
	struct mutex snap_lock;			/* buffer del snapshot */
	enum aw_state state;
	struct net_device *dev;			/* interfaz usada */
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	size_t ssid_len;
	bool secure;				/* WPA2-PSK */
	u8 psk[32];
	u8 bssid[ETH_ALEN];
	u8 rsn[24];				/* IE RSN enviado en la asociacion */
	size_t rsn_len;
	/* handshake */
	u8 anonce[32], snonce[32], ptk[48];
	bool have_ptk;
	u8 last_replay[8];
	bool have_replay;
	char err[48];
	/* cola de EAPOL */
	struct sk_buff_head eapol_q;
	struct work_struct eapol_work;
	/* connect pendiente (los comandos se aceptan en el contexto del write) */
	char *snap;
	size_t snap_len;
} aw;

static void aw_fail(const char *why)
{
	snprintf(aw.err, sizeof(aw.err), "%s", why);
	aw.state = AW_FAILED;
	pr_info("aurora-wifi: fallo: %s\n", why);
}

/* ------------------------------------------------------------------ */
/* Interfaz y redes                                                   */
/* ------------------------------------------------------------------ */

/* Primera interfaz Wi-Fi con netdev. Llamar con rtnl tomado. */
static struct cfg80211_registered_device *aw_find_iface(struct net_device **devp)
{
	struct cfg80211_registered_device *rdev;
	struct wireless_dev *wdev;

	list_for_each_entry(rdev, &cfg80211_rdev_list, list) {
		list_for_each_entry(wdev, &rdev->wiphy.wdev_list, list) {
			if (wdev->netdev && wdev->iftype == NL80211_IFTYPE_STATION) {
				*devp = wdev->netdev;
				return rdev;
			}
		}
	}
	return NULL;
}

enum aw_sec { SEC_OPEN, SEC_WPA2, SEC_WPA2_MFP, SEC_WPA3, SEC_WPA, SEC_WEP, SEC_ENT, SEC_OTHER };
static const char *const aw_sec_name[] = {
	"open", "wpa2", "wpa2-mfp", "wpa3", "wpa", "wep", "802.1x", "other" };

/*
 * Clasifica por los IE de la baliza. Para SEC_WPA2 rellena *group (cifrado de
 * grupo del AP) y comprueba que ofrece CCMP en pairwise y AKM PSK (2).
 */
static enum aw_sec aw_classify(const struct cfg80211_bss *bss, u32 *group)
{
	const struct cfg80211_bss_ies *ies;
	const u8 *rsn, *p, *end;
	int n, i, ccmp = 0, psk = 0, sae = 0, ent = 0, psk256 = 0;
	u16 caps = 0;
	enum aw_sec r;

	rcu_read_lock();
	ies = rcu_dereference(bss->ies);
	if (!ies) {
		rcu_read_unlock();
		return SEC_OTHER;
	}
	rsn = cfg80211_find_ie(WLAN_EID_RSN, ies->data, ies->len);
	if (!rsn) {
		r = SEC_OPEN;
		if (bss->capability & WLAN_CAPABILITY_PRIVACY) {
			if (cfg80211_find_vendor_ie(0x0050f2, 1, ies->data, ies->len))
				r = SEC_WPA;
			else
				r = SEC_WEP;
		}
		rcu_read_unlock();
		return r;
	}
	p = rsn + 2;
	end = p + rsn[1];
	r = SEC_OTHER;
	if (end - p < 8 + 2)
		goto out;
	p += 2;				/* version */
	if (group)
		*group = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
	p += 4;
	n = p[0] | (p[1] << 8);
	p += 2;
	if (end - p < n * 4 + 2)
		goto out;
	for (i = 0; i < n; i++, p += 4)
		if (p[0] == 0x00 && p[1] == 0x0f && p[2] == 0xac && p[3] == 4)
			ccmp = 1;
	n = p[0] | (p[1] << 8);
	p += 2;
	if (end - p < n * 4)
		goto out;
	for (i = 0; i < n; i++, p += 4) {
		if (p[0] != 0x00 || p[1] != 0x0f || p[2] != 0xac)
			continue;
		if (p[3] == 2) psk = 1;
		else if (p[3] == 6) psk256 = 1;
		else if (p[3] == 8) sae = 1;
		else if (p[3] == 1 || p[3] == 5) ent = 1;
	}
	if (end - p >= 2)
		caps = p[0] | (p[1] << 8);
	if (psk && ccmp)
		r = (caps & 0x0040 /* MFPR */) ? SEC_WPA2_MFP : SEC_WPA2;
	else if (sae)
		r = SEC_WPA3;
	else if (ent)
		r = SEC_ENT;
	(void)psk256;
out:
	rcu_read_unlock();
	return r;
}

static int aw_bss_ssid(const struct cfg80211_bss *bss, u8 *ssid, size_t *len)
{
	const struct cfg80211_bss_ies *ies;
	const u8 *s;

	*len = 0;
	rcu_read_lock();
	ies = rcu_dereference(bss->ies);
	if (ies) {
		s = cfg80211_find_ie(WLAN_EID_SSID, ies->data, ies->len);
		if (s && s[1] <= IEEE80211_MAX_SSID_LEN) {
			memcpy(ssid, s + 2, s[1]);
			*len = s[1];
		}
	}
	rcu_read_unlock();
	return 0;
}

/* ------------------------------------------------------------------ */
/* Escaneo (como cfg80211_wext_siwscan)                                */
/* ------------------------------------------------------------------ */

static int aw_scan(void)
{
	struct cfg80211_registered_device *rdev;
	struct net_device *dev;
	struct cfg80211_scan_request_int *creq;
	struct wiphy *wiphy;
	int i, n_channels, err;
	enum nl80211_band band;

	rtnl_lock();
	rdev = aw_find_iface(&dev);
	if (!rdev) {
		rtnl_unlock();
		return -ENODEV;
	}
	err = lkpi_netif_wifi_attach(dev);
	if (err || !netif_running(dev)) {
		pr_info("aurora-wifi: no se pudo levantar %s (%d)\n", dev->name, err);
		rtnl_unlock();
		return err ? err : -ENETDOWN;
	}
	wiphy = &rdev->wiphy;
	wiphy_lock(wiphy);
	if (rdev->scan_req || rdev->scan_msg) {
		err = -EBUSY;
		goto out_unlock;
	}
	n_channels = ieee80211_get_num_supported_channels(wiphy);
	creq = kzalloc(struct_size(creq, req.channels, n_channels) + sizeof(struct cfg80211_ssid),
		       GFP_KERNEL);
	if (!creq) {
		err = -ENOMEM;
		goto out_unlock;
	}
	creq->req.wiphy = wiphy;
	creq->req.wdev = dev->ieee80211_ptr;
	creq->req.ssids = (void *)creq + struct_size(creq, req.channels, n_channels);
	creq->req.n_ssids = 1;
	creq->req.scan_start = jiffies;
	i = 0;
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		int j;

		if (!wiphy->bands[band])
			continue;
		for (j = 0; j < wiphy->bands[band]->n_channels; j++) {
			struct ieee80211_channel *chan = &wiphy->bands[band]->channels[j];

			if (chan->flags & IEEE80211_CHAN_DISABLED ||
			    !cfg80211_wdev_channel_allowed(creq->req.wdev, chan))
				continue;
			creq->req.channels[i++] = chan;
		}
	}
	if (!i) {
		kfree(creq);
		err = -EINVAL;
		goto out_unlock;
	}
	creq->req.n_channels = i;
	for (i = 0; i < NUM_NL80211_BANDS; i++)
		if (wiphy->bands[i])
			creq->req.rates[i] = (1 << wiphy->bands[i]->n_bitrates) - 1;
	eth_broadcast_addr(creq->req.bssid);
	rdev->scan_req = creq;
	err = rdev_scan(rdev, creq);
	if (err) {
		rdev->scan_req = NULL;
		kfree(creq);
	} else {
		dev_hold(dev);
		aw.dev = dev;
	}
out_unlock:
	wiphy_unlock(wiphy);
	rtnl_unlock();
	return err;
}

/* ------------------------------------------------------------------ */
/* Conexion                                                            */
/* ------------------------------------------------------------------ */

static int aw_connect(const char *ssid, size_t ssid_len, const char *pass)
{
	struct cfg80211_registered_device *rdev;
	struct net_device *dev;
	struct cfg80211_internal_bss *ib, *best = NULL;
	struct cfg80211_connect_params cp;
	u32 group = 0, best_group = 0;
	enum aw_sec best_sec = SEC_OTHER;
	bool want_secure = pass && pass[0];
	u8 tmp[IEEE80211_MAX_SSID_LEN];
	size_t tl;
	int err;

	if (ssid_len == 0 || ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;
	if (want_secure && (strlen(pass) < 8 || strlen(pass) > 63))
		return -EINVAL;

	pr_info("aurora-wifi: connect '%.*s'\n", (int)ssid_len, ssid);
	rtnl_lock();
	rdev = aw_find_iface(&dev);
	if (!rdev) {
		rtnl_unlock();
		return -ENODEV;
	}
	err = lkpi_netif_wifi_attach(dev);
	pr_info("aurora-wifi: connect: attach=%d\n", err);
	if (err) {
		rtnl_unlock();
		return err;
	}
	spin_lock_bh(&rdev->bss_lock);
	list_for_each_entry(ib, &rdev->bss_list, list) {
		struct cfg80211_bss *b = &ib->pub;
		enum aw_sec sec;

		aw_bss_ssid(b, tmp, &tl);
		if (tl != ssid_len || memcmp(tmp, ssid, ssid_len))
			continue;
		sec = aw_classify(b, &group);
		if (want_secure ? sec != SEC_WPA2 : sec != SEC_OPEN)
			continue;
		if (want_secure && group != WLAN_CIPHER_SUITE_CCMP)
			continue;	/* grupo TKIP: no soportado */
		if (!best || b->signal > best->pub.signal) {
			best = ib;
			best_sec = sec;
			best_group = group;
		}
	}
	spin_unlock_bh(&rdev->bss_lock);
	/* cfg80211_ref_bss() toma bss_lock: no se puede llamar con el tomado (autobloqueo). */
	if (best)
		cfg80211_ref_bss(&rdev->wiphy, &best->pub);
	if (!best) {
		rtnl_unlock();
		return -ENOENT;
	}
	(void)best_sec;

	memset(&cp, 0, sizeof(cp));
	cp.channel = best->pub.channel;
	cp.bssid = best->pub.bssid;
	cp.ssid = ssid;
	cp.ssid_len = ssid_len;
	cp.auth_type = NL80211_AUTHTYPE_AUTOMATIC;
	cp.mfp = NL80211_MFP_NO;
	cp.bg_scan_period = -1;
	if (want_secure) {
		static const u8 rsn_ie[24] = {
			WLAN_EID_RSN, 20, 1, 0,
			0x00, 0x0f, 0xac, 4,	/* grupo CCMP */
			1, 0, 0x00, 0x0f, 0xac, 4,	/* pairwise CCMP */
			1, 0, 0x00, 0x0f, 0xac, 2,	/* AKM PSK */
			0, 0 };
		mutex_lock(&aw.lock);
		memcpy(aw.rsn, rsn_ie, 22);
		aw.rsn_len = 22;
		mutex_unlock(&aw.lock);
		cp.ie = aw.rsn;
		cp.ie_len = aw.rsn_len;
		cp.privacy = true;
		cp.crypto.wpa_versions = NL80211_WPA_VERSION_2;
		cp.crypto.cipher_group = WLAN_CIPHER_SUITE_CCMP;
		cp.crypto.n_ciphers_pairwise = 1;
		cp.crypto.ciphers_pairwise[0] = WLAN_CIPHER_SUITE_CCMP;
		cp.crypto.n_akm_suites = 1;
		cp.crypto.akm_suites[0] = WLAN_AKM_SUITE_PSK;
		cp.crypto.control_port = true;
		cp.crypto.control_port_ethertype = htons(ETH_P_PAE_);
		cp.crypto.control_port_no_encrypt = true;
	}
	(void)best_group;

	mutex_lock(&aw.lock);
	memcpy(aw.ssid, ssid, ssid_len);
	aw.ssid_len = ssid_len;
	aw.secure = want_secure;
	memcpy(aw.bssid, best->pub.bssid, ETH_ALEN);
	aw.have_ptk = false;
	aw.have_replay = false;
	aw.err[0] = 0;
	if (want_secure)
		wpa_pbkdf2_psk(pass, ssid, ssid_len, aw.psk);
	aw.dev = dev;
	aw.state = AW_CONNECTING;
	mutex_unlock(&aw.lock);

	pr_info("aurora-wifi: connect: cfg80211_connect...\n");
	wiphy_lock(&rdev->wiphy);
	err = cfg80211_connect(rdev, dev, &cp, NULL, NULL);
	wiphy_unlock(&rdev->wiphy);
	pr_info("aurora-wifi: connect: cfg80211_connect=%d\n", err);
	cfg80211_put_bss(&rdev->wiphy, &best->pub);
	rtnl_unlock();
	if (err) {
		mutex_lock(&aw.lock);
		aw_fail("cfg80211_connect fallo");
		mutex_unlock(&aw.lock);
	}
	return err;
}

static int aw_disconnect(void)
{
	struct cfg80211_registered_device *rdev;
	struct net_device *dev;
	int err = 0;

	rtnl_lock();
	rdev = aw_find_iface(&dev);
	if (!rdev) {
		rtnl_unlock();
		return -ENODEV;
	}
	wiphy_lock(&rdev->wiphy);
	err = cfg80211_disconnect(rdev, dev, WLAN_REASON_DEAUTH_LEAVING, true);
	wiphy_unlock(&rdev->wiphy);
	rtnl_unlock();
	mutex_lock(&aw.lock);
	aw.state = AW_IDLE;
	aw.have_ptk = false;
	mutex_unlock(&aw.lock);
	lkpi_netif_wifi_link(dev, false);
	return err;
}

/* ------------------------------------------------------------------ */
/* EAPOL: 4-way handshake y clave de grupo                             */
/* ------------------------------------------------------------------ */

static int aw_send_eapol(struct net_device *dev, const u8 *body, size_t len)
{
	struct sk_buff *skb = alloc_skb(ETH_HLEN + len + 32, GFP_KERNEL);
	u8 *p;

	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, 16);
	p = skb_put(skb, ETH_HLEN + len);
	memcpy(p, aw.bssid, ETH_ALEN);
	memcpy(p + ETH_ALEN, dev->dev_addr, ETH_ALEN);
	p[12] = ETH_P_PAE_ >> 8;
	p[13] = ETH_P_PAE_ & 0xff;
	memcpy(p + ETH_HLEN, body, len);
	skb->dev = dev;
	skb->protocol = htons(ETH_P_PAE_);
	skb_reset_mac_header(skb);
	return dev->netdev_ops->ndo_start_xmit(skb, dev) == NETDEV_TX_OK ? 0 : -EIO;
}

static int aw_install_gtk(struct cfg80211_registered_device *rdev, struct net_device *dev,
			  const u8 *gtk, int gtk_len, int idx, const u8 *rsc)
{
	struct key_params kp = { .key = gtk, .key_len = gtk_len, .cipher = WLAN_CIPHER_SUITE_CCMP,
				 .seq = rsc, .seq_len = 6 };
	int err = rdev_add_key(rdev, dev, -1, idx, false, NULL, &kp);

	if (!err)
		err = rdev_set_default_key(rdev, dev, -1, idx, false, true);
	return err;
}

static int rcmp(const u8 *a, const u8 *b)
{
	return memcmp(a, b, 8);
}

/* Descifra los datos de clave (AES key wrap) y extrae la GTK. */
static int aw_get_gtk(const struct wpa_eapol_key *k, u8 *gtk, int *gl, int *idx)
{
	u8 plain[256];
	int n;

	if (!(k->info & WPA_INFO_ENCDATA) || k->data_len < 24 || (k->data_len & 7) ||
	    k->data_len > sizeof(plain) + 8)
		return -1;
	n = k->data_len / 8 - 1;
	if (wpa_aes_unwrap(aw.ptk + 16, n, k->data, plain))
		return -1;
	return wpa_find_gtk(plain, n * 8, gtk, gl, idx);
}

static void aw_eapol_one(struct net_device *dev, const u8 *src, const u8 *buf, size_t len)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(dev->ieee80211_ptr->wiphy);
	struct wpa_eapol_key k;
	u8 out[160], gtk[32], zero[32] = { 0 };
	int gl = 0, gidx = 0;
	size_t ol;

	if (aw.state != AW_HANDSHAKE && aw.state != AW_CONNECTING && aw.state != AW_CONNECTED)
		return;
	if (!aw.secure || memcmp(src, aw.bssid, ETH_ALEN))
		return;
	if (wpa_parse_eapol_key(buf, len, &k) || (k.info & WPA_INFO_VER_MASK) != 2)
		return;
	if (aw.state == AW_CONNECTING)
		aw.state = AW_HANDSHAKE;

	if ((k.info & WPA_INFO_PAIRWISE) && (k.info & WPA_INFO_ACK) && !(k.info & WPA_INFO_MIC)) {
		/* Mensaje 1/4 */
		memcpy(aw.anonce, k.nonce, 32);
		get_random_bytes(aw.snonce, 32);
		wpa_derive_ptk(aw.psk, aw.bssid, dev->dev_addr, aw.anonce, aw.snonce, aw.ptk);
		aw.have_ptk = true;
		memcpy(aw.last_replay, k.replay, 8);
		aw.have_replay = true;
		ol = wpa_build_eapol_key(out, WPA_INFO_PAIRWISE | WPA_INFO_MIC | 2, 0, k.replay,
					 aw.snonce, aw.rsn, aw.rsn_len, aw.ptk);
		aw_send_eapol(dev, out, ol);
		return;
	}
	if (!aw.have_ptk || !(k.info & WPA_INFO_MIC) || !wpa_check_mic(buf, len, aw.ptk))
		return;
	if (aw.have_replay && rcmp(k.replay, aw.last_replay) <= 0)
		return;

	if (k.info & WPA_INFO_PAIRWISE) {
		/* Mensaje 3/4 */
		struct key_params kp = { .key = aw.ptk + 32, .key_len = 16,
					 .cipher = WLAN_CIPHER_SUITE_CCMP };
		struct station_parameters sp = { 0 };
		int err;

		if (!(k.info & WPA_INFO_INSTALL) || memcmp(k.nonce, aw.anonce, 32))
			return;
		if (aw_get_gtk(&k, gtk, &gl, &gidx) || gl != 16) {
			aw_fail("GTK no valida en mensaje 3");
			return;
		}
		memcpy(aw.last_replay, k.replay, 8);
		ol = wpa_build_eapol_key(out, WPA_INFO_PAIRWISE | WPA_INFO_MIC | WPA_INFO_SECURE | 2,
					 0, k.replay, zero, NULL, 0, aw.ptk);
		aw_send_eapol(dev, out, ol);
		err = rdev_add_key(rdev, dev, -1, 0, true, aw.bssid, &kp);
		if (!err)
			err = aw_install_gtk(rdev, dev, gtk, gl, gidx, buf + 65);
		if (!err) {
			sp.sta_flags_mask = BIT(NL80211_STA_FLAG_AUTHORIZED);
			sp.sta_flags_set = BIT(NL80211_STA_FLAG_AUTHORIZED);
			err = rdev_change_station(rdev, dev, aw.bssid, &sp);
		}
		if (err) {
			aw_fail("no se pudieron instalar las claves");
			return;
		}
		aw.state = AW_CONNECTED;
		lkpi_netif_wifi_link(dev, true);
		return;
	}
	/* Clave de grupo 1/2 */
	if (!(k.info & WPA_INFO_ACK) || aw_get_gtk(&k, gtk, &gl, &gidx) || gl != 16)
		return;
	memcpy(aw.last_replay, k.replay, 8);
	aw_install_gtk(rdev, dev, gtk, gl, gidx, buf + 65);
	ol = wpa_build_eapol_key(out, WPA_INFO_MIC | WPA_INFO_SECURE | 2, 0, k.replay, zero,
				 NULL, 0, aw.ptk);
	aw_send_eapol(dev, out, ol);
}

static void aw_eapol_work(struct work_struct *w)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&aw.eapol_q))) {
		/* eth_type_trans() quito la cabecera Ethernet: sigue justo antes de data */
		const u8 *src = skb->data - ETH_HLEN + ETH_ALEN;
		struct net_device *dev = skb->dev;

		/* orden de bloqueos: rtnl -> wiphy -> aw.lock (los eventos de cfg80211
		 * llegan con wiphy tomado y luego toman aw.lock) */
		rtnl_lock();
		wiphy_lock(dev->ieee80211_ptr->wiphy);
		mutex_lock(&aw.lock);
		if (aw.dev == dev)
			aw_eapol_one(dev, src, skb->data, skb->len);
		mutex_unlock(&aw.lock);
		wiphy_unlock(dev->ieee80211_ptr->wiphy);
		rtnl_unlock();
		kfree_skb(skb);
	}
}

extern void (*lkpi_eapol_rx_hook)(struct net_device *dev, struct sk_buff *skb);

static void aw_eapol_rx(struct net_device *dev, struct sk_buff *skb)
{
	if (skb_queue_len(&aw.eapol_q) > 16) {
		kfree_skb(skb);
		return;
	}
	skb_queue_tail(&aw.eapol_q, skb);
	schedule_work(&aw.eapol_work);
}

/* ------------------------------------------------------------------ */
/* Eventos de cfg80211 (llamados desde nl80211_stub.c)                 */
/* ------------------------------------------------------------------ */

void aurora_wifi_connect_result(struct net_device *dev, int status)
{
	mutex_lock(&aw.lock);
	if (dev == aw.dev && (aw.state == AW_CONNECTING || aw.state == AW_HANDSHAKE)) {
		if (status != WLAN_STATUS_SUCCESS) {
			snprintf(aw.err, sizeof(aw.err), "asociacion rechazada (%d)", status);
			aw.state = AW_FAILED;
		} else if (aw.secure) {
			aw.state = AW_HANDSHAKE;	/* el AP inicia el 4-way */
		} else {
			aw.state = AW_CONNECTED;
			lkpi_netif_wifi_link(dev, true);
		}
	}
	mutex_unlock(&aw.lock);
}

void aurora_wifi_disconnected(struct net_device *dev)
{
	mutex_lock(&aw.lock);
	if (dev == aw.dev && (aw.state == AW_CONNECTED || aw.state == AW_HANDSHAKE ||
			      aw.state == AW_CONNECTING)) {
		aw.have_ptk = false;
		if (aw.state == AW_CONNECTED)
			aw.state = AW_IDLE;
		else
			aw_fail("desautenticado durante la conexion");
		lkpi_netif_wifi_link(dev, false);
	}
	mutex_unlock(&aw.lock);
}

void aurora_wifi_timeout(struct net_device *dev, const char *what)
{
	mutex_lock(&aw.lock);
	if (dev == aw.dev && aw.state == AW_CONNECTING)
		aw_fail(what);
	mutex_unlock(&aw.lock);
}

/* ------------------------------------------------------------------ */
/* /dev/aurora-wifi                                                    */
/* ------------------------------------------------------------------ */

#define SNAP_MAX 8192

static void aw_snapshot(void)
{
	struct cfg80211_registered_device *rdev;
	struct net_device *dev;
	struct cfg80211_internal_bss *ib;
	size_t n = 0;
	char *b = aw.snap;
	enum aw_state st;

	rtnl_lock();
	mutex_lock(&aw.lock);
	rdev = aw_find_iface(&dev);
	st = aw.state;
	if (rdev && rdev->scan_req && st == AW_IDLE)
		st = AW_SCANNING;
	n += scnprintf(b + n, SNAP_MAX - n, "state %s\n", aw_state_name[st]);
	if (st == AW_FAILED)
		n += scnprintf(b + n, SNAP_MAX - n, "error %s\n", aw.err);
	if (!rdev) {
		n += scnprintf(b + n, SNAP_MAX - n, "iface none\n");
		mutex_unlock(&aw.lock);
		rtnl_unlock();
		goto done;
	}
	n += scnprintf(b + n, SNAP_MAX - n, "iface %s\n", dev->name);
	if (st >= AW_CONNECTING && st != AW_FAILED)
		n += scnprintf(b + n, SNAP_MAX - n, "ssid %.*s\nbssid %pM\n", (int)aw.ssid_len,
			       aw.ssid, aw.bssid);
	spin_lock_bh(&rdev->bss_lock);
	list_for_each_entry(ib, &rdev->bss_list, list) {
		struct cfg80211_bss *bss = &ib->pub;
		u8 ssid[IEEE80211_MAX_SSID_LEN + 1];
		size_t sl;
		u32 g;

		if (n + 160 > SNAP_MAX)
			break;
		aw_bss_ssid(bss, ssid, &sl);
		ssid[sl] = 0;
		n += scnprintf(b + n, SNAP_MAX - n, "bss %pM %u %d %s %s\n", bss->bssid,
			       bss->channel ? bss->channel->center_freq : 0, bss->signal / 100,
			       aw_sec_name[aw_classify(bss, &g)], ssid);
	}
	spin_unlock_bh(&rdev->bss_lock);
	mutex_unlock(&aw.lock);
	rtnl_unlock();
done:
	aw.snap_len = n;
	aw.snap[n] = 0;
	{
		/* traza en el log solo cuando el estado cambia (diagnostico en hardware real) */
		static unsigned long last_hash;
		unsigned long h = 5381;
		size_t i;

		for (i = 0; i < n; i++)
			h = h * 33 + (unsigned char)b[i];
		if (h != last_hash) {
			last_hash = h;
			pr_info("aurora-wifi: estado:\n%s", aw.snap);
		}
	}
}

static ssize_t aw_read(struct file *f, char __user *to, size_t count, loff_t *ppos)
{
	ssize_t r;

	mutex_lock(&aw.snap_lock);
	if (*ppos == 0)
		aw_snapshot();
	r = simple_read_from_buffer(to, count, ppos, aw.snap, aw.snap_len);
	mutex_unlock(&aw.snap_lock);
	return r;
}

static ssize_t aw_write(struct file *f, const char __user *from, size_t count, loff_t *ppos)
{
	char buf[160];
	char *ssid, *pass, *nl;
	int err = 0;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, from, count))
		return -EFAULT;
	buf[count] = 0;
	nl = strchr(buf, '\n');
	if (nl)
		*nl = 0;
	if (!strcmp(buf, "scan")) {
		err = aw_scan();
	} else if (!strcmp(buf, "disconnect")) {
		err = aw_disconnect();
	} else if (!strncmp(buf, "connect\t", 8)) {
		ssid = buf + 8;
		pass = strchr(ssid, '\t');
		if (pass)
			*pass++ = 0;
		else
			pass = "";
		err = aw_connect(ssid, strlen(ssid), pass);
	} else {
		err = -EINVAL;
	}
	return err ? err : (ssize_t)count;
}

static const struct file_operations aw_fops = {
	.owner = THIS_MODULE,
	.read = aw_read,
	.write = aw_write,
	.llseek = noop_llseek,
};

static struct miscdevice aw_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "aurora-wifi",
	.fops = &aw_fops,
};

int aurora_wifi_init(void)
{
	mutex_init(&aw.lock);
	mutex_init(&aw.snap_lock);
	skb_queue_head_init(&aw.eapol_q);
	INIT_WORK(&aw.eapol_work, aw_eapol_work);
	aw.snap = kzalloc(SNAP_MAX, GFP_KERNEL);
	if (!aw.snap)
		return -ENOMEM;
	lkpi_eapol_rx_hook = aw_eapol_rx;
	return misc_register(&aw_misc);
}

void aurora_wifi_exit(void)
{
	lkpi_eapol_rx_hook = NULL;
	misc_deregister(&aw_misc);
	cancel_work_sync(&aw.eapol_work);
	skb_queue_purge(&aw.eapol_q);
	kfree(aw.snap);
}
