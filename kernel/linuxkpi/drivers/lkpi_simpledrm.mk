# lkpi_simpledrm.kur: drivers/gpu/drm/sysfb/simpledrm.c de Linux sobre lkpi_drm.kur.
# Linux crea el dispositivo "simple-framebuffer" desde screen_info (sysfb); aqui
# lo crea glue.c con los datos del GOP (modo y direccion) que da el kernel. El
# framebuffer del firmware es una sombra (display-shadow) y la pantalla la
# presenta el compositor.
include kernel/linuxkpi/drivers/lkpi_drm_cflags.mk
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/gpu/drm/sysfb
LKPI_DRV_SRCS   := simpledrm.c drm_sysfb.c drm_sysfb_modeset.c
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_simpledrm/glue.c
LKPI_DRV_CFLAGS := $(LKPI_DRM_CFLAGS) -I$(LKPI_LINUX)/drivers/gpu/drm
LKPI_DRV_CFLAGS_simpledrm.c := '-Dmodule_init(f)=int lkpi_simpledrm_driver_init(void) { return f(); }' \
                               '-Dmodule_exit(f)=void lkpi_simpledrm_driver_exit(void) { f(); }'
LKPI_DRV_INSTALL := linuxkpi
