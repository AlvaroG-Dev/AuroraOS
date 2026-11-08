# alx.kur: drivers/net/ethernet/atheros/alx de Linux (Qualcomm/Atheros AR816x/AR817x,
# Killer E2200/E2400/E2500) sobre LinuxKPI-NET.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/ethernet/atheros/alx
LKPI_DRV_SRCS   := main.c ethtool.c hw.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/ethernet/atheros/alx -include linux/hrtimer.h
LKPI_DRV_INSTALL := linuxkpi
