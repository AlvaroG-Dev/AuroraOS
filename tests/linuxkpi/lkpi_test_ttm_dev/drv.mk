# L6: /dev/lkpi_ttm, el driver de memoria simulado de lkpi_test_ttm expuesto al
# espacio de usuario (ioctl y mmap con ttm_bo_vm). Lo carga y prueba
# user/musl/apps/lkpi_test.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_ttm
LKPI_DRV_SRCS   := test_ttm.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi -Itests/linuxkpi/lkpi_test_ttm -funsigned-char -Wno-pointer-sign \
                   -DKT_DEVICE=1 -DCONFIG_DRM=1 -DCONFIG_DRM_TTM=1 -DCONFIG_DRM_SCHED=1 \
                   -DCONFIG_DRM_GEM_SHMEM_HELPER=1
LKPI_DRV_INSTALL := linuxkpi/tests
