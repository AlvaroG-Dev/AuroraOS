# Pruebas de LinuxKPI de L4 en el kernel: DMA (bounce, tambien con edu),
# scatterlist, dma_pool, vmap, dma_buf, firmware, i2c-algo-bit, debugfs,
# backlight y task_struct.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_k6
LKPI_DRV_SRCS   := test_k6.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
