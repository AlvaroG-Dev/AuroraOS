# lkpi_virtio.kur: el nucleo virtio de Linux (drivers/virtio) sobre la capa
# (L5): bus virtio, virtqueues (split y packed), transporte virtio-pci
# moderno y legacy, dma-buf de objetos virtio. Sin admin queue, vDPA, MMIO
# ni debugfs. Cada fichero tiene su module_init; glue.c los encadena.
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/virtio
LKPI_DRV_SRCS   := virtio.c virtio_ring.c virtio_anchor.c virtio_dma_buf.c \
                   virtio_pci_common.c virtio_pci_modern.c virtio_pci_modern_dev.c \
                   virtio_pci_legacy.c virtio_pci_legacy_dev.c
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_virtio/glue.c
LKPI_DRV_CFLAGS := -DCONFIG_VIRTIO_ANCHOR=1 -DCONFIG_VIRTIO_PCI_LEGACY=1 -DCONFIG_VIRTIO_PCI_ADMIN_LEGACY=0 \
                   -Ikernel/linuxkpi/drivers/lkpi_virtio
LKPI_DRV_CFLAGS_virtio.c := '-Dcore_initcall(f)=int lkpi_virtio_core_init(void) { return f(); }' \
                            '-Dmodule_exit(f)=void lkpi_virtio_core_exit(void) { f(); }'
LKPI_DRV_CFLAGS_virtio_pci_common.c := '-Dmodule_init(f)=int lkpi_virtio_pci_init(void) { return f(); }' \
                            '-Dmodule_exit(f)=void lkpi_virtio_pci_exit(void) { f(); }'
