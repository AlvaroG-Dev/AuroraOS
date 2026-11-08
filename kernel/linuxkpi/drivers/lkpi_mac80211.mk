# mac80211.kur: net/mac80211 de Linux sobre LinuxKPI + cfg80211 (Plan_Red.md, N7.2).
# Sin debugfs, LEDs, mesh ni PM. Rate control minstrel_ht incluido.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/net/mac80211
LKPI_DRV_SRCS   := main.c status.c driver-ops.c sta_info.c wep.c aead_api.c wpa.c scan.c offchannel.c ht.c agg-tx.c agg-rx.c vht.c he.c s1g.c ibss.c iface.c link.c rate.c tkip.c aes_cmac.c aes_gmac.c fils_aead.c cfg.c ethtool.c rx.c spectmgmt.c tx.c key.c util.c parse.c wme.c chan.c mlme.c tdls.c ocb.c airtime.c eht.c rc80211_minstrel_ht.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/net/mac80211 -I$(LKPI_LINUX)/net/wireless -include linux/hrtimer.h -include linux/netdevice.h -DCONFIG_CFG80211=1 -DCONFIG_MAC80211=1 -DCONFIG_MAC80211_RC_MINSTREL=1 -DCONFIG_MAC80211_HAS_RC=1 -DCONFIG_MAC80211_STA_HASH_MAX_SIZE=0 -DCONFIG_MAC80211_RC_DEFAULT=\"minstrel_ht\"
LKPI_DRV_SEDEXPR := s/control\.jiffies/control.lkpi_ctl_jiffies/g
LKPI_DRV_INSTALL := linuxkpi
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_mac80211/stubs.c
