# lkpi_ttm.kur: TTM, el asignador buddy de GPU y el planificador de GPU de
# Linux (drivers/gpu/drm/ttm, drivers/gpu/buddy.c + drm_buddy.c y
# drivers/gpu/drm/scheduler) sobre la capa (L6). Depende de lkpi_drm.kur
# (drm_device, GEM, dma_resv, vma_node...). Sin AGP, sin KUnit.
include kernel/linuxkpi/drivers/lkpi_drm_cflags.mk
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/gpu
LKPI_DRV_SRCS   := buddy.c drm/drm_buddy.c \
                   drm/ttm/ttm_tt.c drm/ttm/ttm_bo.c drm/ttm/ttm_bo_util.c drm/ttm/ttm_bo_vm.c \
                   drm/ttm/ttm_module.c drm/ttm/ttm_execbuf_util.c drm/ttm/ttm_range_manager.c \
                   drm/ttm/ttm_resource.c drm/ttm/ttm_pool.c drm/ttm/ttm_device.c \
                   drm/ttm/ttm_sys_manager.c drm/ttm/ttm_backup.c \
                   drm/scheduler/sched_main.c drm/scheduler/sched_fence.c drm/scheduler/sched_entity.c
LKPI_DRV_CFLAGS := $(LKPI_DRM_CFLAGS) -Wno-format-security -Wno-maybe-uninitialized -DCONFIG_DRM_TTM=1 -DCONFIG_DRM_SCHED=1 \
                   -I$(LKPI_LINUX)/drivers/gpu/drm -I$(LKPI_LINUX)/drivers/gpu/drm/ttm \
                   -I$(LKPI_LINUX)/drivers/gpu/drm/scheduler
LKPI_DRV_INSTALL := linuxkpi
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_ttm/glue.c
# Un solo kur_init: los module_init de Linux se renombran y los llama glue.c.
LKPI_DRV_CFLAGS_buddy.c := '-Dmodule_init(f)=int lkpi_gpu_buddy_init(void) { return f(); }' \
                           '-Dmodule_exit(f)=void lkpi_gpu_buddy_exit(void) { f(); }'
LKPI_DRV_CFLAGS_sched_fence.c := '-Dmodule_init(f)=int lkpi_sched_fence_init(void) { return f(); }' \
                                 '-Dmodule_exit(f)=void lkpi_sched_fence_exit(void) { f(); }'
