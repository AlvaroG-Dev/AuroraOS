// SPDX-License-Identifier: GPL-2.0
/*
 * Sustituto de net/wireless/nl80211.c (Plan_Red.md, N7.1). AuroraOS no tiene
 * generic netlink: cfg80211 notifica eventos (escaneo, asociacion, ...) y aqui
 * se descartan. El control desde espacio de usuario vendra de una interfaz
 * propia (N7.3), no de nl80211.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/cfg80211.h>
#include "nl80211.h"
#include "core.h"

int nl80211_init(void) { return 0; }
void nl80211_exit(void) { }
void *nl80211hdr_put(struct sk_buff *skb, u32 portid, u32 seq, int flags, u8 cmd) { return NULL; }
bool nl80211_put_sta_rate(struct sk_buff *msg, struct rate_info *info, int attr) { return false; }
int nl80211_parse_chandef(struct cfg80211_registered_device *rdev, struct netlink_ext_ack *extack,
			  struct nlattr **attrs, struct cfg80211_chan_def *chandef) { return -EOPNOTSUPP; }
int nl80211_parse_random_mac(struct nlattr **attrs, u8 *mac_addr, u8 *mac_addr_mask) { return -EOPNOTSUPP; }
void nl80211_notify_wiphy(struct cfg80211_registered_device *rdev, enum nl80211_commands cmd) { }
void nl80211_notify_iface(struct cfg80211_registered_device *rdev, struct wireless_dev *wdev, enum nl80211_commands cmd) { }
void nl80211_send_scan_start(struct cfg80211_registered_device *rdev, struct wireless_dev *wdev) { }
struct sk_buff *nl80211_build_scan_msg(struct cfg80211_registered_device *rdev, struct wireless_dev *wdev, bool aborted) { return NULL; }
void nl80211_send_scan_msg(struct cfg80211_registered_device *rdev, struct sk_buff *msg) { }
void nl80211_send_sched_scan(struct cfg80211_sched_scan_request *req, u32 cmd) { }
void nl80211_common_reg_change_event(enum nl80211_commands cmd_id, struct regulatory_request *request) { }
void nl80211_send_rx_auth(struct cfg80211_registered_device *rdev, struct net_device *netdev, const u8 *buf, size_t len, gfp_t gfp) { }
void nl80211_send_rx_assoc(struct cfg80211_registered_device *rdev, struct net_device *netdev, const struct cfg80211_rx_assoc_resp_data *data) { }
void nl80211_send_deauth(struct cfg80211_registered_device *rdev, struct net_device *netdev, const u8 *buf, size_t len, bool reconnect, gfp_t gfp) { }
void nl80211_send_disassoc(struct cfg80211_registered_device *rdev, struct net_device *netdev, const u8 *buf, size_t len, bool reconnect, gfp_t gfp) { }
void nl80211_send_auth_timeout(struct cfg80211_registered_device *rdev, struct net_device *netdev, const u8 *addr, gfp_t gfp) { }
void nl80211_send_assoc_timeout(struct cfg80211_registered_device *rdev, struct net_device *netdev, const u8 *addr, gfp_t gfp) { }
void nl80211_send_connect_result(struct cfg80211_registered_device *rdev, struct net_device *netdev, struct cfg80211_connect_resp_params *params, gfp_t gfp) { }
void nl80211_send_roamed(struct cfg80211_registered_device *rdev, struct net_device *netdev, struct cfg80211_roam_info *info, gfp_t gfp) { }
void nl80211_send_port_authorized(struct cfg80211_registered_device *rdev, struct net_device *netdev, const u8 *peer_addr, const u8 *td_bitmap, u8 td_bitmap_len) { }
void nl80211_send_disconnected(struct cfg80211_registered_device *rdev, struct net_device *netdev, u16 reason, const u8 *ie, size_t ie_len, bool from_ap) { }
void nl80211_michael_mic_failure(struct cfg80211_registered_device *rdev, struct net_device *netdev, const u8 *addr, enum nl80211_key_type key_type, int key_id, const u8 *tsc, gfp_t gfp) { }
void nl80211_send_beacon_hint_event(struct wiphy *wiphy, struct ieee80211_channel *channel_before, struct ieee80211_channel *channel_after) { }
void nl80211_send_ibss_bssid(struct cfg80211_registered_device *rdev, struct net_device *netdev, const u8 *bssid, gfp_t gfp) { }
int nl80211_send_mgmt(struct cfg80211_registered_device *rdev, struct wireless_dev *wdev, u32 nlpid, struct cfg80211_rx_info *info, gfp_t gfp) { return -EOPNOTSUPP; }
void nl80211_radar_notify(struct cfg80211_registered_device *rdev, const struct cfg80211_chan_def *chandef, enum nl80211_radar_event event, struct net_device *netdev, gfp_t gfp) { }
void nl80211_send_ap_stopped(struct wireless_dev *wdev, unsigned int link_id) { }
void cfg80211_free_coalesce(struct cfg80211_coalesce *coalesce) { kfree(coalesce); }
void nl80211_mlo_reconf_add_done(struct net_device *dev, struct cfg80211_mlo_reconf_done_data *data) { }
void cfg80211_cqm_rssi_notify_work(struct wiphy *wiphy, struct wiphy_work *work) { }
void cfg80211_schedule_channels_check(struct wireless_dev *wdev) { }

/* Notificaciones de nl80211.c: se descartan (N7.1) */
void cfg80211_assoc_comeback(struct net_device *netdev,
			     const u8 *ap_addr, u32 timeout) { }
int cfg80211_bss_color_notify(struct net_device *dev,
			      enum nl80211_commands cmd, u8 count,
			      u64 color_bitmap, u8 link_id) { return 0; }
void cfg80211_ch_switch_notify(struct net_device *dev,
			       struct cfg80211_chan_def *chandef,
			       unsigned int link_id) { }
void cfg80211_ch_switch_started_notify(struct net_device *dev,
				       struct cfg80211_chan_def *chandef,
				       unsigned int link_id, u8 count,
				       bool quiet) { }
int cfg80211_check_station_change(struct wiphy *wiphy,
				  struct station_parameters *params,
				  enum cfg80211_station_type statype) { return 0; }
void cfg80211_control_port_tx_status(struct wireless_dev *wdev, u64 cookie,
				     const u8 *buf, size_t len, bool ack,
				     gfp_t gfp) { }
void cfg80211_cqm_beacon_loss_notify(struct net_device *dev, gfp_t gfp) { }
void cfg80211_cqm_pktloss_notify(struct net_device *dev,
				 const u8 *peer, u32 num_packets, gfp_t gfp) { }
void cfg80211_del_sta_sinfo(struct net_device *dev, const u8 *mac_addr,
			    struct station_info *sinfo, gfp_t gfp) { }
void cfg80211_epcs_changed(struct net_device *netdev, bool enabled) { }
void cfg80211_gtk_rekey_notify(struct net_device *dev, const u8 *bssid,
			       const u8 *replay_ctr, gfp_t gfp) { }
void cfg80211_links_removed(struct net_device *dev, u16 link_mask) { }
void cfg80211_mgmt_tx_status_ext(struct wireless_dev *wdev,
				 struct cfg80211_tx_status *status, gfp_t gfp) { }
void cfg80211_nan_func_terminated(struct wireless_dev *wdev,
				  u8 inst_id,
				  enum nl80211_nan_func_term_reason reason,
				  u64 cookie, gfp_t gfp) { }
void cfg80211_nan_match(struct wireless_dev *wdev,
			struct cfg80211_nan_match_params *match, gfp_t gfp) { }
void cfg80211_new_sta(struct net_device *dev, const u8 *mac_addr,
		      struct station_info *sinfo, gfp_t gfp) { }
void cfg80211_probe_status(struct net_device *dev, const u8 *addr,
			   u64 cookie, bool acked, s32 ack_signal,
			   bool is_valid_ack_signal, gfp_t gfp) { }
void cfg80211_ready_on_channel(struct wireless_dev *wdev, u64 cookie,
			       struct ieee80211_channel *chan,
			       unsigned int duration, gfp_t gfp) { }
void cfg80211_remain_on_channel_expired(struct wireless_dev *wdev, u64 cookie,
					struct ieee80211_channel *chan,
					gfp_t gfp) { }
void cfg80211_report_obss_beacon_khz(struct wiphy *wiphy, const u8 *frame,
				     size_t len, int freq, int sig_dbm) { }
bool cfg80211_rx_control_port(struct net_device *dev, struct sk_buff *skb,
			      bool unencrypted, int link_id) { return false; }
bool cfg80211_rx_spurious_frame(struct net_device *dev, const u8 *addr,
				int link_id, gfp_t gfp) { return false; }
bool cfg80211_rx_unexpected_4addr_frame(struct net_device *dev, const u8 *addr,
					int link_id, gfp_t gfp) { return false; }
void cfg80211_rx_unprot_mlme_mgmt(struct net_device *dev,
				  const u8 *buf, size_t len) { }
void cfg80211_sta_opmode_change_notify(struct net_device *dev, const u8 *mac,
				       struct sta_opmode_info *sta_opmode,
				       gfp_t gfp) { }
void cfg80211_tdls_oper_request(struct net_device *dev, const u8 *peer,
				enum nl80211_tdls_operation oper,
				u16 reason_code, gfp_t gfp) { }
void cfg80211_tx_mgmt_expired(struct wireless_dev *wdev, u64 cookie,
			      struct ieee80211_channel *chan, gfp_t gfp) { }

void cfg80211_cqm_rssi_notify(struct net_device *dev, enum nl80211_cqm_rssi_threshold_event rssi_event, s32 rssi_level, gfp_t gfp) { }
EXPORT_SYMBOL(cfg80211_assoc_comeback);
EXPORT_SYMBOL(cfg80211_bss_color_notify);
EXPORT_SYMBOL(cfg80211_ch_switch_notify);
EXPORT_SYMBOL(cfg80211_ch_switch_started_notify);
EXPORT_SYMBOL(cfg80211_check_station_change);
EXPORT_SYMBOL(cfg80211_control_port_tx_status);
EXPORT_SYMBOL(cfg80211_cqm_beacon_loss_notify);
EXPORT_SYMBOL(cfg80211_cqm_pktloss_notify);
EXPORT_SYMBOL(cfg80211_cqm_rssi_notify);
EXPORT_SYMBOL(cfg80211_del_sta_sinfo);
EXPORT_SYMBOL(cfg80211_epcs_changed);
EXPORT_SYMBOL(cfg80211_gtk_rekey_notify);
EXPORT_SYMBOL(cfg80211_links_removed);
EXPORT_SYMBOL(cfg80211_mgmt_tx_status_ext);
EXPORT_SYMBOL(cfg80211_nan_func_terminated);
EXPORT_SYMBOL(cfg80211_nan_match);
EXPORT_SYMBOL(cfg80211_new_sta);
EXPORT_SYMBOL(cfg80211_probe_status);
EXPORT_SYMBOL(cfg80211_ready_on_channel);
EXPORT_SYMBOL(cfg80211_remain_on_channel_expired);
EXPORT_SYMBOL(cfg80211_report_obss_beacon_khz);
EXPORT_SYMBOL(cfg80211_rx_control_port);
EXPORT_SYMBOL(cfg80211_rx_spurious_frame);
EXPORT_SYMBOL(cfg80211_rx_unexpected_4addr_frame);
EXPORT_SYMBOL(cfg80211_rx_unprot_mlme_mgmt);
EXPORT_SYMBOL(cfg80211_sta_opmode_change_notify);
EXPORT_SYMBOL(cfg80211_tdls_oper_request);
EXPORT_SYMBOL(cfg80211_tx_mgmt_expired);
EXPORT_SYMBOL(cfg80211_schedule_channels_check);
