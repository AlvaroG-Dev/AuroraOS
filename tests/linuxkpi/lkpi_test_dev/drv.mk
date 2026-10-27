# Hito de L4: driver de prueba con /dev/lkpi_test (ioctl, read/write/llseek,
# mmap de DMA y por fallo de pagina, poll/epoll, fasync, firmware, dma_buf y
# senales). Lo carga y prueba user/musl/apps/lkpi_test.
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_dev
LKPI_DRV_SRCS   := test_dev.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
