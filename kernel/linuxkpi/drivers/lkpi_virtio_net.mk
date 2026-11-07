# virtio_net.kur: drivers/net/virtio_net.c de Linux sobre lkpi_virtio (NIC virtio de
# QEMU/KVM/VirtualBox/cloud). Sin XDP, page_pool ni DIM.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net
LKPI_DRV_SRCS   := virtio_net.c
LKPI_DRV_CFLAGS := -include linux/hrtimer.h
LKPI_DRV_INSTALL := linuxkpi
