# Pruebas de LinuxKPI de L6: ttm, drm_buddy y gpu-sched (lkpi_ttm.kur sobre
# lkpi_drm.kur) con un driver de memoria de dispositivo simulado.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_ttm
LKPI_DRV_SRCS   := test_ttm.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi -funsigned-char -Wno-pointer-sign \
                   -DCONFIG_DRM=1 -DCONFIG_DRM_TTM=1 -DCONFIG_DRM_SCHED=1 -DCONFIG_DRM_GEM_SHMEM_HELPER=1
LKPI_DRV_INSTALL := linuxkpi/tests
