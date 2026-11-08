// SPDX-License-Identifier: GPL-2.0
/*
 * wlansim: adaptador Wi-Fi simulado para probar cfg80211 + mac80211 + la capa
 * en QEMU (que no emula ningun chip Wi-Fi). Hace de "hardware": anuncia tres
 * redes (beacons y probe responses) y contesta auth/assoc de la abierta, como
 * mac80211_hwsim pero sin netlink. No va en el autoarranque: se carga con
 * lkpi.load=...,lkpi_wlansim.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <net/mac80211.h>

struct sim_ap {
	const char *ssid;
	u8 bssid[ETH_ALEN];
	int chan;	/* 1..11 */
	bool rsn;
	s8 dbm;
};

static const struct sim_ap aps[] = {
	{ "AuroraSim",      { 0x02, 0, 0, 0xac, 0xab, 1 }, 6,  false, -45 },
	{ "AuroraWPA2",     { 0x02, 0, 0, 0xac, 0xab, 2 }, 1,  true,  -60 },
	{ "AuroraRedAbierta", { 0x02, 0, 0, 0xac, 0xab, 3 }, 11, false, -75 },
};

static struct ieee80211_hw *g_hw;
static int g_freq;
static unsigned long g_data_dropped;

static struct ieee80211_channel sim_chans[11];
static struct ieee80211_rate sim_rates[] = {
	{ .bitrate = 10, .hw_value = 0 }, { .bitrate = 20, .hw_value = 1 },
	{ .bitrate = 55, .hw_value = 2 }, { .bitrate = 110, .hw_value = 3 },
};
static struct ieee80211_supported_band sim_band = {
	.band = NL80211_BAND_2GHZ,
	.channels = sim_chans, .n_channels = ARRAY_SIZE(sim_chans),
	.bitrates = sim_rates, .n_bitrates = ARRAY_SIZE(sim_rates),
};

static int ap_freq(const struct sim_ap *ap) { return 2407 + 5 * ap->chan; }

static void sim_deliver(struct sk_buff *skb, int freq, s8 dbm)
{
	struct ieee80211_rx_status *rs = IEEE80211_SKB_RXCB(skb);

	memset(rs, 0, sizeof(*rs));
	rs->freq = freq;
	rs->band = NL80211_BAND_2GHZ;
	rs->signal = dbm;
	rs->rate_idx = 0;
	rs->nss = 1;
	ieee80211_rx_irqsafe(g_hw, skb);
}

static u8 *put_ie(u8 *p, u8 id, const void *d, u8 len)
{
	*p++ = id; *p++ = len;
	memcpy(p, d, len);
	return p + len;
}

/* Beacon (stype=BEACON) o probe response (PROBE_RESP) de `ap` hacia `da`. */
static void sim_send_beacon(const struct sim_ap *ap, u16 stype, const u8 *da)
{
	static const u8 rates[] = { 0x82, 0x84, 0x8b, 0x96 };
	static const u8 rsn[] = { 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
				  1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
	struct ieee80211_mgmt *m;
	struct sk_buff *skb;
	u8 *p, ch = ap->chan;

	skb = alloc_skb(sizeof(*m) + 128, GFP_ATOMIC);
	if (!skb)
		return;
	m = skb_put_zero(skb, offsetof(struct ieee80211_mgmt, u.beacon.variable));
	m->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | stype);
	ether_addr_copy(m->da, da);
	ether_addr_copy(m->sa, ap->bssid);
	ether_addr_copy(m->bssid, ap->bssid);
	m->u.beacon.beacon_int = cpu_to_le16(100);
	m->u.beacon.capab_info = cpu_to_le16(WLAN_CAPABILITY_ESS |
					     (ap->rsn ? WLAN_CAPABILITY_PRIVACY : 0));
	p = skb_put(skb, 2 + strlen(ap->ssid) + 2 + sizeof(rates) + 3 + 2 + sizeof(rsn));
	p = put_ie(p, WLAN_EID_SSID, ap->ssid, strlen(ap->ssid));
	p = put_ie(p, WLAN_EID_SUPP_RATES, rates, sizeof(rates));
	p = put_ie(p, WLAN_EID_DS_PARAMS, &ch, 1);
	if (ap->rsn)
		put_ie(p, WLAN_EID_RSN, rsn, sizeof(rsn));
	else
		skb_trim(skb, skb->len - (2 + sizeof(rsn)));
	sim_deliver(skb, ap_freq(ap), ap->dbm);
}

static void sim_beacons_on(int freq)
{
	static const u8 bc[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	int i;

	for (i = 0; i < ARRAY_SIZE(aps); i++)
		if (ap_freq(&aps[i]) == freq)
			sim_send_beacon(&aps[i], IEEE80211_STYPE_BEACON, bc);
}

static const struct sim_ap *find_ap(const u8 *bssid)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(aps); i++)
		if (ether_addr_equal(aps[i].bssid, bssid))
			return &aps[i];
	return NULL;
}

static void sim_reply_auth(const struct sim_ap *ap, const struct ieee80211_mgmt *rq)
{
	struct ieee80211_mgmt *m;
	struct sk_buff *skb = alloc_skb(sizeof(*m), GFP_ATOMIC);

	if (!skb)
		return;
	m = skb_put_zero(skb, offsetof(struct ieee80211_mgmt, u.auth.variable));
	m->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_AUTH);
	ether_addr_copy(m->da, rq->sa);
	ether_addr_copy(m->sa, ap->bssid);
	ether_addr_copy(m->bssid, ap->bssid);
	m->u.auth.auth_alg = cpu_to_le16(WLAN_AUTH_OPEN);
	m->u.auth.auth_transaction = cpu_to_le16(2);
	m->u.auth.status_code = cpu_to_le16(WLAN_STATUS_SUCCESS);
	pr_info("wlansim: auth de %pM -> %s\n", rq->sa, ap->ssid);
	sim_deliver(skb, ap_freq(ap), ap->dbm);
}

static void sim_reply_assoc(const struct sim_ap *ap, const struct ieee80211_mgmt *rq)
{
	static const u8 rates[] = { 0x82, 0x84, 0x8b, 0x96 };
	struct ieee80211_mgmt *m;
	struct sk_buff *skb = alloc_skb(sizeof(*m) + 16, GFP_ATOMIC);
	u8 *p;

	if (!skb)
		return;
	m = skb_put_zero(skb, offsetof(struct ieee80211_mgmt, u.assoc_resp.variable));
	m->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ASSOC_RESP);
	ether_addr_copy(m->da, rq->sa);
	ether_addr_copy(m->sa, ap->bssid);
	ether_addr_copy(m->bssid, ap->bssid);
	m->u.assoc_resp.capab_info = cpu_to_le16(WLAN_CAPABILITY_ESS);
	m->u.assoc_resp.status_code = cpu_to_le16(WLAN_STATUS_SUCCESS);
	m->u.assoc_resp.aid = cpu_to_le16(0xc001);
	p = skb_put(skb, 2 + sizeof(rates));
	put_ie(p, WLAN_EID_SUPP_RATES, rates, sizeof(rates));
	pr_info("wlansim: assoc de %pM -> %s\n", rq->sa, ap->ssid);
	sim_deliver(skb, ap_freq(ap), ap->dbm);
}

static void sim_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
		   struct sk_buff *skb)
{
	struct ieee80211_mgmt *m = (void *)skb->data;
	const struct sim_ap *ap;

	if (ieee80211_is_mgmt(m->frame_control)) {
		if (ieee80211_is_probe_req(m->frame_control)) {
			int i;

			for (i = 0; i < ARRAY_SIZE(aps); i++)
				if (ap_freq(&aps[i]) == g_freq)
					sim_send_beacon(&aps[i], IEEE80211_STYPE_PROBE_RESP, m->sa);
		} else if (ieee80211_is_auth(m->frame_control)) {
			ap = find_ap(m->da);
			if (ap && !ap->rsn)
				sim_reply_auth(ap, m);
		} else if (ieee80211_is_assoc_req(m->frame_control)) {
			ap = find_ap(m->da);
			if (ap && !ap->rsn)
				sim_reply_assoc(ap, m);
		}
	} else {
		g_data_dropped++;
	}
	ieee80211_free_txskb(hw, skb);
}

static void sim_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
	struct sk_buff *skb;

	while ((skb = ieee80211_tx_dequeue(hw, txq))) {
		g_data_dropped++;
		ieee80211_free_txskb(hw, skb);
	}
}

static int sim_start(struct ieee80211_hw *hw) { return 0; }
static void sim_stop(struct ieee80211_hw *hw, bool suspend) { }
static int sim_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif) { return 0; }
static void sim_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif) { }

static int sim_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		g_freq = hw->conf.chandef.chan ? hw->conf.chandef.chan->center_freq : 0;
		if (g_freq)
			sim_beacons_on(g_freq);
	}
	return 0;
}

static void sim_configure_filter(struct ieee80211_hw *hw, unsigned int changed,
				 unsigned int *total, u64 mc)
{
	*total = 0;
}

static int sim_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       unsigned int link_id, u16 ac,
		       const struct ieee80211_tx_queue_params *params)
{
	return 0;
}

static int sim_sta_state(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta, enum ieee80211_sta_state o,
			 enum ieee80211_sta_state n)
{
	pr_info("wlansim: sta %pM estado %d -> %d\n", sta->addr, o, n);
	return 0;
}

static void sim_bss_info_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				 struct ieee80211_bss_conf *info, u64 changed)
{
	if (changed & BSS_CHANGED_ASSOC)
		pr_info("wlansim: assoc=%d bssid=%pM\n", vif->cfg.assoc, info->bssid);
}

static int sim_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
		       struct ieee80211_vif *vif, struct ieee80211_sta *sta,
		       struct ieee80211_key_conf *key)
{
	return -EOPNOTSUPP;	/* cifrado por software en mac80211 */
}

static int sim_set_rts_threshold(struct ieee80211_hw *hw, int radio_idx, u32 value)
{
	return 0;
}

static const struct ieee80211_ops sim_ops = {
	.set_rts_threshold = sim_set_rts_threshold,
	.tx = sim_tx,
	.wake_tx_queue = sim_wake_tx_queue,
	.start = sim_start,
	.stop = sim_stop,
	.add_interface = sim_add_interface,
	.remove_interface = sim_remove_interface,
	.config = sim_config,
	.configure_filter = sim_configure_filter,
	.conf_tx = sim_conf_tx,
	.sta_state = sim_sta_state,
	.bss_info_changed = sim_bss_info_changed,
	.set_key = sim_set_key,
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
};

static int __init wlansim_init(void)
{
	static const u8 addr[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x57, 0x4c, 0x01 };
	int i, err;

	for (i = 0; i < ARRAY_SIZE(sim_chans); i++) {
		sim_chans[i].band = NL80211_BAND_2GHZ;
		sim_chans[i].center_freq = 2412 + 5 * i;
		sim_chans[i].hw_value = i + 1;
		sim_chans[i].max_power = 20;
	}
	g_hw = ieee80211_alloc_hw(0, &sim_ops);
	if (!g_hw) {
		pr_err("wlansim: ieee80211_alloc_hw fallo\n");
		return -ENOMEM;
	}
	g_hw->wiphy->bands[NL80211_BAND_2GHZ] = &sim_band;
	g_hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	SET_IEEE80211_PERM_ADDR(g_hw, addr);
	ieee80211_hw_set(g_hw, SIGNAL_DBM);
	ieee80211_hw_set(g_hw, HAS_RATE_CONTROL);
	g_hw->queues = 4;
	g_hw->max_rates = 1;
	g_hw->max_rate_tries = 1;
	err = ieee80211_register_hw(g_hw);
	if (err) {
		pr_err("wlansim: ieee80211_register_hw -> %d\n", err);
		ieee80211_free_hw(g_hw);
		g_hw = NULL;
		return err;
	}
	pr_info("wlansim: adaptador simulado registrado (%zu redes)\n", ARRAY_SIZE(aps));
	return 0;
}

static void __exit wlansim_exit(void)
{
	if (g_hw) {
		ieee80211_unregister_hw(g_hw);
		ieee80211_free_hw(g_hw);
	}
}

module_init(wlansim_init);
module_exit(wlansim_exit);
MODULE_DESCRIPTION("Adaptador Wi-Fi simulado para pruebas de la capa LinuxKPI");
MODULE_LICENSE("GPL");
