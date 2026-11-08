# e1000.kur: drivers/net/ethernet/intel/e1000 de Linux (Intel PRO/1000 PCI/PCI-X:
# 82540EM de QEMU y VirtualBox, 82545, 82546, 82541, 82547...) sobre LinuxKPI-NET.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/ethernet/intel/e1000
LKPI_DRV_SRCS   := e1000_main.c e1000_hw.c e1000_ethtool.c e1000_param.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/ethernet/intel/e1000
LKPI_DRV_INSTALL := linuxkpi
