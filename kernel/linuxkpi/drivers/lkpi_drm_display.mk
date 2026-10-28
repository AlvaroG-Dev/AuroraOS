# lkpi_drm_display.kur: los helpers de pantalla de DRM de Linux que usa i915
# (drivers/gpu/drm/display: DisplayPort, MST, DSC, HDCP, HDMI, SCDC, LSPCON) mas
# drm_panel y drm_mipi_dsi (L7). Depende de lkpi_drm.kur. Sin CEC, sin
# aux-dev/aux-bus, sin tunel DP ni audio/estado HDMI.
include kernel/linuxkpi/drivers/lkpi_drm_cflags.mk
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/gpu/drm
LKPI_DRV_SRCS   := display/drm_dp_helper.c display/drm_dp_mst_topology.c \
                   display/drm_dp_dual_mode_helper.c display/drm_dsc_helper.c \
                   display/drm_hdcp_helper.c display/drm_hdmi_helper.c display/drm_scdc_helper.c \
                   drm_panel.c drm_mipi_dsi.c
LKPI_DRV_CFLAGS := $(LKPI_DRM_CFLAGS) -DCONFIG_DRM_PANEL=1 -Wno-format-security -Wno-maybe-uninitialized \
                   -I$(LKPI_LINUX)/drivers/gpu/drm -I$(LKPI_LINUX)/drivers/gpu/drm/display
LKPI_DRV_INSTALL := linuxkpi
LKPI_DRV_CFLAGS += -include lkpi/gpu_prelude.h
