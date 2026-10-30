# Pruebas de LinuxKPI de L5 en el kernel: K7 (dma_fence, chain, array, unwrap,
# dma_resv, sync_file y el enlace con los fences de Aurora).
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_k7
LKPI_DRV_SRCS   := test_k7.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
