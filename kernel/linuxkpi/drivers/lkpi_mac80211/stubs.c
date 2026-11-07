// SPDX-License-Identifier: GPL-2.0
/* Piezas de mac80211 que AuroraOS no usa (WBRF de AMD, drop reasons). */
#include <linux/kernel.h>
#include <linux/module.h>
#include <net/mac80211.h>
#include <net/dropreason.h>
#include "ieee80211_i.h"

void ieee80211_check_wbrf_support(struct ieee80211_local *local) { }
void ieee80211_add_wbrf(struct ieee80211_local *local, struct cfg80211_chan_def *chandef) { }
void ieee80211_remove_wbrf(struct ieee80211_local *local, struct cfg80211_chan_def *chandef) { }

void drop_reasons_register_subsys(enum skb_drop_reason_subsys subsys, const struct drop_reason_list *list) { }
void drop_reasons_unregister_subsys(enum skb_drop_reason_subsys subsys) { }
const struct drop_reason_list __rcu *drop_reasons_by_subsys[SKB_DROP_REASON_SUBSYS_NUM];
