# tg3.kur: drivers/net/ethernet/broadcom/tg3.c de Linux (Broadcom NetXtreme BCM57xx
# Gigabit: portatiles y servidores Dell/HP/Lenovo) sobre LinuxKPI-NET. Sin hwmon ni PTP.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/ethernet/broadcom
LKPI_DRV_SRCS   := tg3.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/ethernet/broadcom -include linux/hrtimer.h
LKPI_DRV_INSTALL := linuxkpi
