# cfg80211.kur: net/wireless de Linux sobre LinuxKPI (Plan_Red.md, fase N7.1).
# Sin nl80211/wext/debugfs/regdb firmada: nl80211.c se sustituye por un stub local.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/net/wireless
LKPI_DRV_SRCS   := core.c sysfs.c radiotap.c util.c reg.c scan.c mlme.c ibss.c sme.c chan.c ethtool.c mesh.c ap.c ocb.c michael-mic.c pmsr.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/net/wireless -include linux/hrtimer.h -include linux/netdevice.h -DCONFIG_CFG80211=1
LKPI_DRV_INSTALL := linuxkpi
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_cfg80211/nl80211_stub.c kernel/linuxkpi/drivers/lkpi_cfg80211/aurora_wifi.c kernel/linuxkpi/drivers/lkpi_cfg80211/wpa_psk.c
