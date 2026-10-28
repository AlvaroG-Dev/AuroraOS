# Opciones comunes a lkpi_drm.kur y a los drivers DRM de Linux que cuelgan de el.
LKPI_DRM_CFLAGS := -funsigned-char -Wno-pointer-sign -DCONFIG_DRM=1 -DCONFIG_DRM_KMS_HELPER=1 \
                   -DCONFIG_DRM_CLIENT=1 -DCONFIG_DRM_CLIENT_SETUP=1 '-DCONFIG_DRM_CLIENT_DEFAULT=""' \
                   -DCONFIG_DEBUG_FS=1 -DCONFIG_MMU=1 -DCONFIG_DRM_GEM_SHMEM_HELPER=1 \
                   -DCONFIG_APERTURE_HELPERS=1 -DCONFIG_PCI=1 -DCONFIG_PM_SLEEP=1 -DCONFIG_PM=1
