# bochs.kur: drivers/gpu/drm/tiny/bochs.c de Linux (QEMU -vga std, 1234:1111)
# sobre lkpi_drm.kur. Arrancar con cmdline=aur.skip=1234:1111 lkpi.load=... para
# que no lo tome bochs.aur. El framebuffer y los registros son una sombra del
# GOP (display-shadow); la pantalla la presenta el compositor.
include kernel/linuxkpi/drivers/lkpi_drm_cflags.mk
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/gpu/drm/tiny
LKPI_DRV_SRCS   := bochs.c
LKPI_DRV_CFLAGS := $(LKPI_DRM_CFLAGS) -I$(LKPI_LINUX)/drivers/gpu/drm
LKPI_DRV_INSTALL := linuxkpi
