# r8169.kur: drivers/net/ethernet/realtek/r8169 de Linux (RTL8101/8111/8168/8125)
# sobre la capa LinuxKPI-NET (Plan_Red.md, fase N4).
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/ethernet/realtek
LKPI_DRV_SRCS   := r8169_main.c r8169_firmware.c r8169_phy_config.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/ethernet/realtek
LKPI_DRV_INSTALL := linuxkpi
