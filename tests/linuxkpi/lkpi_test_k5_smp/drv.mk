# Pruebas de LinuxKPI, nivel K5 (L3): drivers PCI de Linux de juguete sobre
# edu y virtio-rng-pci de QEMU, con varias CPU (TEST_FLAG_NEEDS_SMP).
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_k5_smp
LKPI_DRV_SRCS   := test_k5_smp.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
