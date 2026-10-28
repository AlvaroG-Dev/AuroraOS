# Pruebas de LinuxKPI de L5: el nucleo DRM/KMS de Linux (lkpi_drm.kur) con un
# driver DRM de juguete (pipe simple, GEM shmem) escrito contra la API de Linux.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_drm
LKPI_DRV_SRCS   := test_drm.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi -funsigned-char -Wno-pointer-sign \
                   -DCONFIG_DRM=1 -DCONFIG_DRM_KMS_HELPER=1 -DCONFIG_DRM_GEM_SHMEM_HELPER=1
LKPI_DRV_INSTALL := linuxkpi/tests
