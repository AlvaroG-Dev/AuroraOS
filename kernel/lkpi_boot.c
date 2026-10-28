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

#include "lkpi_boot.h"
#include "cmdline.h"
#include "klog.h"
#include "kur_loader.h"
#include "string.h"

#define LKPI_BOOT_DIR "/lib/modules/linuxkpi/"

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
    if (*p) {
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
