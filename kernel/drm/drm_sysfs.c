// kernel/drm/drm_sysfs.c
//
// /sys/class/drm, /sys/dev/char/226:N y la base de datos de udev
// (/run/udev/data/c226:N) que libudev lee para etiquetar los nodos DRM con
// el seat (Weston/wlroots/logind los buscan asi).
//
// Los nodos cuelgan de un dispositivo "platform" (aurora-drm): libdrm solo
// enumera (drmGetDevices2) los nodos cuyo padre expone `subsystem` y `uevent`.

#include "../nl_uevent.h"
#include "../sysfs_dyn.h"
#include "../string.h"
#include "../vfs.h"
#include "drm.h"

#define DRM_PARENT "devices/platform/aurora-drm"
#define DRM_DEV_BASE DRM_PARENT "/drm"

static void publish_node(const char *name, int minor) {
  char p[128], v[160];
  snprintf(p, sizeof(p), DRM_DEV_BASE "/%s/dev", name);
  snprintf(v, sizeof(v), "%d:%d\n", DRM_MAJOR, minor);
  sysfs_dyn_set_file(p, v);
  snprintf(p, sizeof(p), DRM_DEV_BASE "/%s/uevent", name);
  snprintf(v, sizeof(v), "MAJOR=%d\nMINOR=%d\nDEVNAME=dri/%s\nDEVTYPE=drm_minor\n",
           DRM_MAJOR, minor, name);
  sysfs_dyn_set_file(p, v);
  snprintf(p, sizeof(p), DRM_DEV_BASE "/%s/subsystem", name);
  sysfs_dyn_add_symlink(p, "/sys/class/drm");
  snprintf(p, sizeof(p), DRM_DEV_BASE "/%s/device", name);
  sysfs_dyn_add_symlink(p, "/sys/" DRM_PARENT);

  snprintf(p, sizeof(p), "class/drm/%s", name);
  snprintf(v, sizeof(v), "/sys/" DRM_DEV_BASE "/%s", name);
  sysfs_dyn_add_symlink(p, v);
  snprintf(p, sizeof(p), "dev/char/%d:%d", DRM_MAJOR, minor);
  sysfs_dyn_add_symlink(p, v);
}

void drm_sysfs_publish(void) {
  // Dispositivo padre (platform).
  sysfs_dyn_set_file(DRM_PARENT "/uevent",
                     "DRIVER=aurora-drm\nOF_NAME=aurora-drm\n"
                     "OF_FULLNAME=/aurora-drm\nOF_COMPATIBLE_0=aurora,drm\n"
                     "OF_COMPATIBLE_N=1\nMODALIAS=platform:aurora-drm\n");
  sysfs_dyn_set_file(DRM_PARENT "/modalias", "platform:aurora-drm\n");
  sysfs_dyn_add_symlink(DRM_PARENT "/subsystem", "/sys/bus/platform");

  publish_node("card0", DRM_CARD_MINOR);
  publish_node("renderD128", DRM_RENDER_MINOR);

  // Conector virtual: /sys/class/drm/card0-Virtual-1
  const char *c = DRM_DEV_BASE "/card0/card0-Virtual-1";
  char p[160];
  snprintf(p, sizeof(p), "%s/status", c);
  sysfs_dyn_set_file(p, "connected\n");
  snprintf(p, sizeof(p), "%s/enabled", c);
  sysfs_dyn_set_file(p, "enabled\n");
  snprintf(p, sizeof(p), "%s/dpms", c);
  sysfs_dyn_set_file(p, "On\n");
  snprintf(p, sizeof(p), "%s/subsystem", c);
  sysfs_dyn_add_symlink(p, "/sys/class/drm");
  sysfs_dyn_add_symlink("class/drm/card0-Virtual-1", "/sys/" DRM_DEV_BASE "/card0/card0-Virtual-1");
}

static void write_text(const char *path, const char *text) {
  vfs_create(path, 0644);
  vfs_node_t *n = vfs_lookup(path);
  if (!n)
    return;
  if (n->ops && n->ops->write)
    n->ops->write(n, 0, strlen(text), text);
  vfs_node_free(n);
}

void drm_udev_publish(void) {
  vfs_mkdir("/run/udev", 0755);
  vfs_mkdir("/run/udev/data", 0755);
  static const char card[] =
      "I:1\nE:ID_PATH=platform-aurora-drm\nE:ID_PATH_TAG=platform-aurora-drm\n"
      "E:ID_FOR_SEAT=drm-platform-aurora-drm\n"
      "G:seat\nG:master-of-seat\nG:uaccess\nQ:seat\nQ:master-of-seat\nQ:uaccess\n"
      "V:1\n";
  static const char render[] =
      "I:1\nE:ID_PATH=platform-aurora-drm\nE:ID_PATH_TAG=platform-aurora-drm\n"
      "G:uaccess\nQ:uaccess\nV:1\n";
  write_text("/run/udev/data/c226:0", card);
  write_text("/run/udev/data/c226:128", render);
  write_text("/run/udev/data/+drm:card0", card);
  write_text("/run/udev/data/+drm:renderD128", render);
}
