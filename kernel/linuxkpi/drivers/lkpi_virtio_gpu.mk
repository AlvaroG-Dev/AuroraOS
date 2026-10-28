# lkpi_virtio_gpu.kur: drivers/gpu/drm/virtio de Linux (virtio-gpu 2D, y 3D/venus
# si el host lo ofrece) sobre lkpi_virtio.kur y lkpi_drm.kur. Arrancar con
# cmdline=aur.skip=1af4:1050 lkpi.load=linuxkpi,lkpi_virtio,lkpi_drm,lkpi_virtio_gpu.
include kernel/linuxkpi/drivers/lkpi_drm_cflags.mk
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/gpu/drm/virtio
LKPI_DRV_SRCS   := virtgpu_drv.c virtgpu_kms.c virtgpu_gem.c virtgpu_vram.c virtgpu_display.c \
                   virtgpu_vq.c virtgpu_fence.c virtgpu_object.c virtgpu_debugfs.c virtgpu_plane.c \
                   virtgpu_ioctl.c virtgpu_prime.c virtgpu_trace_points.c virtgpu_submit.c
LKPI_DRV_CFLAGS := $(LKPI_DRM_CFLAGS) -DCONFIG_DRM_VIRTIO_GPU_KMS=1 -I$(LKPI_LINUX)/drivers/gpu/drm
LKPI_DRV_INSTALL := linuxkpi
