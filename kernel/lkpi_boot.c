// kernel/lkpi_boot.c
//
// Carga de la capa LinuxKPI y de drivers de Linux al arrancar (L5).
//
//   cmdline=lkpi.load=linuxkpi,lkpi_virtio,lkpi_drm,lkpi_bochs
//
// Cada nombre es /lib/modules/linuxkpi/<nombre>.kur, en ese orden (la capa
// primero; los .kur se enlazan con los exports de los anteriores). Sin
// `lkpi.load` no se carga nada: la capa y sus pruebas siguen siendo cosa de
// la suite y del espacio de usuario (finit_module).

#include <stdbool.h>

#include "lkpi_boot.h"
#include "cmdline.h"
#include "klog.h"
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

int lkpi_boot_load(void) {
  char list[256];
  if (!cmdline_get("lkpi.load", list, sizeof(list)) || !list[0])
    return 0;

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
  return loaded;
}
