# Estres de ttm con varios hilos compitiendo por el VRAM (mismas fuentes que
# lkpi_test_ttm, con KT_SMP=1).
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_ttm
LKPI_DRV_SRCS   := test_ttm.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi -funsigned-char -Wno-pointer-sign -DKT_SMP=1 \
                   -DCONFIG_DRM=1 -DCONFIG_DRM_TTM=1 -DCONFIG_DRM_SCHED=1 -DCONFIG_DRM_GEM_SHMEM_HELPER=1
LKPI_DRV_INSTALL := linuxkpi/tests
