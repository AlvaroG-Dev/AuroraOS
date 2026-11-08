# igb.kur: drivers/net/ethernet/intel/igb de Linux (82575/82576/82580/I350/I354/I210/I211,
# placas con puertos Gigabit Intel) sobre LinuxKPI-NET. Sin hwmon, debugfs ni SR-IOV.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/ethernet/intel/igb
LKPI_DRV_SRCS   := igb_main.c igb_ethtool.c e1000_82575.c e1000_mac.c e1000_nvm.c e1000_phy.c e1000_mbx.c e1000_i210.c
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_igb/ptp_stub.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/ethernet/intel/igb -include linux/hrtimer.h
LKPI_DRV_INSTALL := linuxkpi
