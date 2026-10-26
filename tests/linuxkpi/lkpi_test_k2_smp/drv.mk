# Estres de LinuxKPI K2 con varias CPU (hito de L2). Se registra con
# TEST_FLAG_NEEDS_SMP; pensado para -smp 4.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_k2_smp
LKPI_DRV_SRCS   := test_k2_smp.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
