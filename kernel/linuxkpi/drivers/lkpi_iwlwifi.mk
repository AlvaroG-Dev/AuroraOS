# iwlwifi.kur: nucleo + bus PCIe de Intel Wi-Fi (Plan_Red.md, N7.3). Lo usa
# lkpi_iwlmvm.kur (op_mode MVM: 7260 ... AX201/AX200/AX210/AX211). Sin debugfs,
# trazas, ACPI/UEFI ni MEI.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/wireless/intel/iwlwifi
LKPI_DRV_SRCS   := iwl-io.c iwl-drv.c iwl-debug.c iwl-nvm-utils.c iwl-utils.c iwl-phy-db.c iwl-nvm-parse.c \
  pcie/ctxt-info.c pcie/ctxt-info-v2.c pcie/drv.c pcie/utils.c \
  pcie/gen1_2/rx.c pcie/gen1_2/tx.c pcie/gen1_2/trans.c pcie/gen1_2/trans-gen2.c pcie/gen1_2/tx-gen2.c \
  cfg/7000.c cfg/8000.c cfg/9000.c cfg/22000.c cfg/ax210.c cfg/bz.c cfg/sc.c cfg/rf-jf.c cfg/rf-hr.c cfg/rf-gf.c \
  iwl-dbg-tlv.c iwl-trans.c fw/img.c fw/notif-wait.c fw/rs.c fw/dbg.c fw/pnvm.c fw/dump.c fw/regulatory.c \
  fw/paging.c fw/smem.c fw/init.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/wireless/intel/iwlwifi -include linux/hrtimer.h -include linux/netdevice.h \
  -DCONFIG_IWLWIFI=1 -DCONFIG_IWLMVM=1 -DCONFIG_IWLWIFI_OPMODE_MODULAR=1 -DCONFIG_CFG80211=1 -DCONFIG_MAC80211=1 -Wno-override-init
LKPI_DRV_INSTALL := linuxkpi
