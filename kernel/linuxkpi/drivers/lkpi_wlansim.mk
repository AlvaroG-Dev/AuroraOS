# wlansim.kur: adaptador Wi-Fi simulado (prueba de cfg80211 + mac80211 en QEMU).
# No se autocarga: lkpi.load=...,lkpi_cfg80211,lkpi_mac80211,lkpi_wlansim
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net
LKPI_DRV_SRCS   :=
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_wlansim/wlansim.c
LKPI_DRV_CFLAGS := -include linux/hrtimer.h -DCONFIG_MAC80211=1 -DCONFIG_CFG80211=1
LKPI_DRV_INSTALL := linuxkpi
