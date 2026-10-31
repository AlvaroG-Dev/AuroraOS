// kernel/lkpi_boot.c
//
// Carga de la capa LinuxKPI y de drivers de Linux al arrancar (L5).
//
//   cmdline=lkpi.load=linuxkpi,lkpi_virtio,lkpi_drm,lkpi_bochs
//
// Cada nombre es /lib/modules/linuxkpi/<nombre>.kur, en ese orden (la capa
// primero; los .kur se enlazan con los exports de los anteriores). Sin
// `lkpi.load` se detecta el hardware, como hace Linux: una GPU Intel (PCI
// 8086, clase VGA) carga la pila i915 sola. `lkpi.auto=off` lo desactiva y
// `lkpi.load=` explicito manda siempre. Si no hay nada que cargar, la capa y
// sus pruebas siguen siendo cosa de la suite y del espacio de usuario
// (finit_module).

#include <stdbool.h>

#include "lkpi_boot.h"
#include "cmdline.h"
#include "klog.h"
#include "pci.h"
#include "kur_loader.h"
#include "string.h"

#define LKPI_BOOT_DIR "/lib/modules/linuxkpi/"

// DRM_BACKEND=native (make) o drm.backend=native (cmdline): el backend nativo
// sirve siempre /dev/dri y los modulos DRM de Linux no se cargan.
static bool drm_native_only(void) {
#ifdef DRM_BACKEND_NATIVE
  return true;
#else
  char v[16];
  return cmdline_get("drm.backend", v, sizeof(v)) && !strcmp(v, "native");
#endif
}

static bool is_drm_module(const char *name) {
  static const char *const drm[] = {"lkpi_drm", "lkpi_bochs", "lkpi_simpledrm",
                                    "lkpi_virtio_gpu"};
  for (unsigned i = 0; i < sizeof(drm) / sizeof(drm[0]); i++)
    if (!strcmp(name, drm[i]))
      return true;
  return false;
}

#define LKPI_AUTO_INTEL                                                        \
  "linuxkpi,lkpi_drm,lkpi_ttm,lkpi_drm_display,lkpi_i915"
// NVIDIA (open-gpu-kernel-modules): RM + nvkms + nvidia-drm (make nvidia).
#define LKPI_AUTO_NVIDIA                                                       \
  "linuxkpi,lkpi_drm,nvidia,nvidia-modeset,nvidia-drm"

// Lista de modulos para el hardware presente, o NULL si no hay ninguno que
// tenga driver de Linux en la capa.
static const char *lkpi_autodetect(void) {
  char v[16];
  if (cmdline_get("lkpi.auto", v, sizeof(v)) && !strcmp(v, "off"))
    return NULL;
  uint8_t bus = 0, slot = 0, func = 0;	// 0:0.0 es el puente de host
  pci_device_t dev;
  while (pci_find_next_device(0x03, 0x00, &bus, &slot, &func, &dev) == 0) {
    if (dev.vendor_id == 0x8086) {
      LOG_INFO("[LKPI] GPU Intel %04x:%04x detectada: pila i915", dev.vendor_id,
               dev.device_id);
      return LKPI_AUTO_INTEL;
    }
    if (dev.vendor_id == 0x10de) {
      LOG_INFO("[LKPI] GPU NVIDIA %04x:%04x detectada: pila nvidia", dev.vendor_id,
               dev.device_id);
      return LKPI_AUTO_NVIDIA;
    }
  }
  return NULL;
}

static int boot_loaded;

int lkpi_boot_loaded(void) { return boot_loaded; }

int lkpi_boot_load(void) {
  char list[256];
  if (!cmdline_get("lkpi.load", list, sizeof(list)) || !list[0]) {
    const char *auto_list = drm_native_only() ? NULL : lkpi_autodetect();
    if (!auto_list)
      return 0;
    snprintf(list, sizeof(list), "%s", auto_list);
  }

  int loaded = 0;
  for (char *p = list; *p;) {
    char *e = p;
    while (*e && *e != ',')
      e++;
    char sep = *e;
    *e = '\0';
    if (*p && is_drm_module(p) && drm_native_only()) {
      LOG_INFO("[LKPI] %s omitido (DRM_BACKEND=native)", p);
    } else if (*p) {
      char path[128];
      snprintf(path, sizeof(path), LKPI_BOOT_DIR "%s.kur", p);
      struct kur_module *m = NULL;
      int rc = kur_loader_load_file(path, &m);
      if (rc == 0) {
        LOG_INFO("[LKPI] %s cargado", p);
        loaded++;
      } else {
        LOG_ERR("[LKPI] %s no carga (rc=%d)", p, rc);
      }
    }
    p = sep ? e + 1 : e;
  }
  boot_loaded = loaded > 0;
  return loaded;
}
