# Pruebas de LinuxKPI de L5: el nucleo virtio de Linux (lkpi_virtio.kur) con un
# driver virtio de juguete sobre virtio-rng-pci.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_virtio
LKPI_DRV_SRCS   := test_virtio.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
