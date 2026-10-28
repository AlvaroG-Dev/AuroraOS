# lkpi_i915.kur: el driver i915 de Linux (drivers/gpu/drm/i915) sobre la capa (L7).
# La lista de fuentes sale del Makefile de Linux (scripts/lkpi-kbuild-srcs.py) con
# X86, ACPI, DEBUG_FS y CAPTURE_ERROR; sin GVT, selftests, PERF_EVENTS (i915_pmu),
# HWMON, FBDEV_EMULATION, PXP ni DP_TUNNEL. Depende de lkpi_drm.kur, lkpi_ttm.kur y
# lkpi_drm_display.kur. No se carga solo: hace falta lkpi.load= explicito (Plan §11, L7).
include kernel/linuxkpi/drivers/lkpi_drm_cflags.mk
include kernel/linuxkpi/drivers/lkpi_i915_cflags.mk
LKPI_DRV_SRCDIR := $(LKPI_LINUX)/drivers/gpu/drm/i915
LKPI_DRV_SRCS   := $(shell cat kernel/linuxkpi/drivers/lkpi_i915.srcs)
LKPI_DRV_CFLAGS := $(LKPI_DRM_CFLAGS) $(LKPI_I915_CFLAGS) -DCONFIG_DRM_TTM=1 -DCONFIG_DRM_SCHED=1 \
                   -Wno-format-security -Wno-maybe-uninitialized -Wno-address-of-packed-member \
                   -I$(LKPI_LINUX)/drivers/gpu/drm -I$(LKPI_LINUX)/drivers/gpu/drm/i915 \
                   -I$(LKPI_LINUX)/drivers/gpu/drm/i915/display
LKPI_DRV_INSTALL := linuxkpi
LKPI_DRV_LOCAL_SRCS := kernel/linuxkpi/drivers/lkpi_i915/glue.c
