/* net/mac80211.h del arbol de Linux. Tiene un campo `unsigned long jiffies` en
 * ieee80211_tx_info.control; aqui `jiffies` es una macro (lkpi_jiffies()), asi que
 * el campo se llama lkpi_ctl_jiffies (mac80211 lo usa como info->control.jiffies:
 * descriptor lkpi_mac80211.mk lo renombra con sed). */
#ifndef _LKPI_NET_MAC80211_WRAP_H
#define _LKPI_NET_MAC80211_WRAP_H
#include <net/cfg80211.h>
#pragma push_macro("jiffies")
#undef jiffies
#define jiffies lkpi_ctl_jiffies
#include <kreal/mac80211.h>
#undef jiffies
#pragma pop_macro("jiffies")
#endif
