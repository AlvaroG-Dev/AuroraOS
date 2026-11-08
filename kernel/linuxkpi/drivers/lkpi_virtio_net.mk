# virtio_net.kur: NIC virtio de QEMU/KVM/VirtualBox/cloud. Driver compacto propio
# (kernel/linuxkpi/drivers/lkpi_virtio_net/): el virtio_net.c de Linux 6.18 necesita
# XDP/AF_XDP/page_pool/DIM/failover. Va sobre lkpi_virtio.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/net
LKPI_DRV_SRCS   :=
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_virtio_net/virtio_net_lkpi.c
LKPI_DRV_CFLAGS := -include linux/hrtimer.h
LKPI_DRV_INSTALL := linuxkpi
