# Pruebas de LinuxKPI de L6 en la capa: shrinker, split_page, "swap" en RAM,
# kmap_local_page_prot, io_mapping, mmu_notifier (stub) y hmm desactivado.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_k9
LKPI_DRV_SRCS   := test_k9.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
