# cfg80211.kur: net/wireless de Linux sobre LinuxKPI (Plan_Red.md, fase N7.1).
# Sin nl80211/wext/debugfs/regdb firmada: nl80211.c se sustituye por un stub local.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/net/wireless
LKPI_DRV_SRCS   := core.c sysfs.c radiotap.c util.c reg.c scan.c mlme.c ibss.c sme.c chan.c ethtool.c mesh.c ap.c ocb.c michael-mic.c pmsr.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/net/wireless
LKPI_DRV_INSTALL := linuxkpi
