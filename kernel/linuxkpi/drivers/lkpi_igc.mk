# igc.kur: drivers/net/ethernet/intel/igc de Linux (I225/I226) sobre LinuxKPI-NET
# (Plan_Red.md, fase N5). Sin PTP (stub local); XDP/AF_XDP son stubs en include/linuxkpi.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/ethernet/intel/igc
LKPI_DRV_SRCS   := igc_main.c igc_base.c igc_mac.c igc_i225.c igc_nvm.c igc_phy.c igc_diag.c igc_ethtool.c igc_dump.c igc_tsn.c igc_xdp.c
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_igc/igc_ptp_stub.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/ethernet/intel/igc
LKPI_DRV_INSTALL := linuxkpi
