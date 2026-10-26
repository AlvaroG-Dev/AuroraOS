# Pruebas unitarias de LinuxKPI, nivel K2 (L2): sincronizacion, tiempo y
# trabajo diferido. Validas con una o varias CPU.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_k2
LKPI_DRV_SRCS   := test_k2.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
