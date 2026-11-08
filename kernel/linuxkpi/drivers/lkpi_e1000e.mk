# e1000e.kur: drivers/net/ethernet/intel/e1000e de Linux (82574L de QEMU, I217/I218/I219
# del PCH y demas) sobre LinuxKPI-NET (Plan_Red.md, fase N3). Sustituye a e1000e.aur:
# arrancar con aur.skip=8086:10d3 lkpi.load=linuxkpi,lkpi_e1000e.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/ethernet/intel/e1000e
LKPI_DRV_SRCS   := 82571.c ich8lan.c 80003es2lan.c mac.c manage.c nvm.c phy.c param.c ethtool.c netdev.c ptp.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/ethernet/intel/e1000e
LKPI_DRV_INSTALL := linuxkpi
