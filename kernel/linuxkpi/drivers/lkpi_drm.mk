# lkpi_drm.kur: el nucleo DRM/KMS de Linux (drivers/gpu/drm) sobre la capa
# (L5): drm_drv/drm_file/drm_ioctl, KMS atomico y sus helpers, GEM con el
# helper shmem, PRIME, syncobj, vblank, EDID y modos. Sin fbdev, sin
# paneles/bridges de DT, sin TTM/DMA/VRAM helpers ni drm_panic. Las cabeceras
# include/drm y uapi/drm vienen del arbol (scripts/lkpi-ext-inc.sh).
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/gpu/drm
LKPI_DRV_SRCS   := $(shell cat kernel/linuxkpi/drivers/lkpi_drm.srcs)
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_drm/glue.c \
                       $(LKPI_LINUX)/drivers/video/hdmi.c $(LKPI_LINUX)/drivers/video/cmdline.c \
                       $(LKPI_LINUX)/drivers/video/aperture.c
LKPI_DRV_CFLAGS := -funsigned-char -Wno-pointer-sign -DCONFIG_DRM=1 -DCONFIG_DRM_KMS_HELPER=1 -DCONFIG_DRM_CLIENT=1 \
                   -DCONFIG_DRM_CLIENT_SETUP=1 '-DCONFIG_DRM_CLIENT_DEFAULT=""' \
                   -DCONFIG_DEBUG_FS=1 -DCONFIG_APERTURE_HELPERS=1 -DCONFIG_PCI=1 -DCONFIG_MMU=1 -DCONFIG_DRM_GEM_SHMEM_HELPER=1 \
                   -Ikernel/linuxkpi/drivers/lkpi_drm
LKPI_DRV_CFLAGS_drm_drv.c := '-Dmodule_init(f)=int lkpi_drm_core_init(void) { return f(); }' \
                             '-Dmodule_exit(f)=void lkpi_drm_core_exit(void) { f(); }'
LKPI_DRV_CFLAGS_drm_mipi_dsi.c := '-Dpostcore_initcall(f)=int lkpi_drm_dsi_init(void) { return f(); }'
LKPI_DRV_CFLAGS_drm_atomic_helper.c := -include kernel/linuxkpi/drivers/lkpi_drm/hooks.h
LKPI_DRV_CFLAGS_drm_drv.c += -include kernel/linuxkpi/drivers/lkpi_drm/hooks.h
