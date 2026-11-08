# Pruebas de LinuxKPI de L9.1: IOMMU/VT-d (dominio privado por dispositivo,
# dma_map con IOVA, permisos R/W, fallos de traduccion). Necesita QEMU con
# `-machine q35 -device intel-iommu,caching-mode=on` y `iommu=on` en la linea
# del kernel; sin eso se omite.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_iommu
LKPI_DRV_SRCS   := test_iommu.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
