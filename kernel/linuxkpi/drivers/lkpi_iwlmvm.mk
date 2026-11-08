# iwlmvm.kur: op_mode MVM de iwlwifi sobre mac80211 (Plan_Red.md, N7.3).
# Sin debugfs, LEDs, PM (d3), MEI ni vendor commands.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net/wireless/intel/iwlwifi/mvm
LKPI_DRV_SRCS   := fw.c mac80211.c nvm.c ops.c phy-ctxt.c mac-ctxt.c utils.c rx.c rxmq.c tx.c binding.c quota.c sta.c sf.c \
  scan.c time-event.c rs.c rs-fw.c power.c coex.c tt.c offloading.c tdls.c ftm-responder.c ftm-initiator.c rfi.c \
  mld-key.c mld-mac.c link.c mld-sta.c mld-mac80211.c ptp.c time-sync.c
LKPI_DRV_CFLAGS := -I$(LKPI_LINUX)/drivers/net/wireless/intel/iwlwifi -I$(LKPI_LINUX)/drivers/net/wireless/intel/iwlwifi/mvm \
  -include linux/hrtimer.h -include linux/netdevice.h \
  -DCONFIG_IWLWIFI=1 -DCONFIG_IWLMVM=1 -DCONFIG_IWLWIFI_OPMODE_MODULAR=1 -DCONFIG_CFG80211=1 -DCONFIG_MAC80211=1 -DTRUE=1 -Wno-override-init
LKPI_DRV_SEDEXPR := s/control\.jiffies/control.lkpi_ctl_jiffies/g
LKPI_DRV_INSTALL := linuxkpi
