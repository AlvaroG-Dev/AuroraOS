// tests/wayland/udev_test.c
//
// libudev de la distro (sin tocar) sobre el /sys y /run/udev de AuroraOS:
// enumera el subsistema "input", muestra syspath/devnode/propiedades y el
// padre con los atributos del dispositivo. Es lo que hace libinput antes de
// abrir /dev/input/event*.

#include <libudev.h>
#include <sys/sysmacros.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  struct udev *u = udev_new();
  if (!u) {
    printf("[udev_test] FAIL udev_new\n");
    return 1;
  }
  printf("[udev_test] PASS udev_new\n");

  struct udev_enumerate *e = udev_enumerate_new(u);
  udev_enumerate_add_match_subsystem(e, "input");
  int rc = udev_enumerate_scan_devices(e);
  printf("[udev_test] %s scan_devices rc=%d\n", rc == 0 ? "PASS" : "FAIL", rc);

  int n = 0, ev = 0, kbd = 0, mouse = 0;
  struct udev_list_entry *le;
  udev_list_entry_foreach(le, udev_enumerate_get_list_entry(e)) {
    const char *sp = udev_list_entry_get_name(le);
    struct udev_device *d = udev_device_new_from_syspath(u, sp);
    n++;
    if (!d) {
      printf("[udev_test] FAIL new_from_syspath(%s)\n", sp);
      continue;
    }
    const char *sysname = udev_device_get_sysname(d);
    const char *node = udev_device_get_devnode(d);
    printf("[udev_test]   %s sysname=%s devnode=%s subsystem=%s devpath=%s\n", sp,
           sysname ? sysname : "-", node ? node : "-",
           udev_device_get_subsystem(d) ? udev_device_get_subsystem(d) : "-",
           udev_device_get_devpath(d) ? udev_device_get_devpath(d) : "-");
    if (sysname && !strncmp(sysname, "event", 5)) {
      ev++;
      const char *k = udev_device_get_property_value(d, "ID_INPUT_KEYBOARD");
      const char *m = udev_device_get_property_value(d, "ID_INPUT_MOUSE");
      printf("[udev_test]     ID_INPUT=%s KEYBOARD=%s MOUSE=%s MAJOR=%s MINOR=%s\n",
             udev_device_get_property_value(d, "ID_INPUT")
                 ? udev_device_get_property_value(d, "ID_INPUT")
                 : "-",
             k ? k : "-", m ? m : "-",
             udev_device_get_property_value(d, "MAJOR")
                 ? udev_device_get_property_value(d, "MAJOR")
                 : "-",
             udev_device_get_property_value(d, "MINOR")
                 ? udev_device_get_property_value(d, "MINOR")
                 : "-");
      if (k && !strcmp(k, "1"))
        kbd++;
      if (m && !strcmp(m, "1"))
        mouse++;
      struct udev_device *p =
          udev_device_get_parent_with_subsystem_devtype(d, "input", NULL);
      printf("[udev_test]     parent=%s name=%s\n",
             p ? udev_device_get_syspath(p) : "(none)",
             p && udev_device_get_sysattr_value(p, "name")
                 ? udev_device_get_sysattr_value(p, "name")
                 : "-");
    }
    udev_device_unref(d);
  }
  printf("[udev_test] %s %d dispositivos, %d event*, teclados=%d ratones=%d\n",
         (ev == 2 && kbd == 1 && mouse == 1) ? "PASS" : "FAIL", n, ev, kbd,
         mouse);

  struct udev_device *dn = udev_device_new_from_devnum(u, 'c', makedev(13, 64));
  printf("[udev_test] %s new_from_devnum(13:64) -> %s\n", dn ? "PASS" : "FAIL",
         dn ? udev_device_get_sysname(dn) : "-");

  struct udev_monitor *mon = udev_monitor_new_from_netlink(u, "udev");
  int mrc = mon ? udev_monitor_enable_receiving(mon) : -1;
  printf("[udev_test] %s udev_monitor (rc=%d fd=%d)\n", mrc == 0 ? "PASS" : "FAIL",
         mrc, mon ? udev_monitor_get_fd(mon) : -1);

  // DRM: lo que busca Weston/wlroots (subsistema drm, card[0-9]*, tag seat).
  int cards = 0, seat_ok = 0;
  struct udev_enumerate *de = udev_enumerate_new(u);
  udev_enumerate_add_match_subsystem(de, "drm");
  udev_enumerate_add_match_sysname(de, "card[0-9]*");
  udev_enumerate_scan_devices(de);
  udev_list_entry_foreach(le, udev_enumerate_get_list_entry(de)) {
    struct udev_device *d =
        udev_device_new_from_syspath(u, udev_list_entry_get_name(le));
    if (!d)
      continue;
    const char *node = udev_device_get_devnode(d);
    printf("[udev_test]   drm %s devnode=%s seat_tag=%d\n",
           udev_device_get_sysname(d), node ? node : "-",
           udev_device_has_tag(d, "seat"));
    if (node && !strcmp(node, "/dev/dri/card0")) {
      cards++;
      if (udev_device_has_tag(d, "seat"))
        seat_ok++;
    }
    udev_device_unref(d);
  }
  printf("[udev_test] %s drm card0 (cards=%d seat=%d)\n",
         (cards == 1 && seat_ok == 1) ? "PASS" : "FAIL", cards, seat_ok);

  int ok = (ev == 2 && kbd == 1 && mouse == 1 && dn && mrc == 0 && cards == 1 &&
            seat_ok == 1);
  printf("[udev_test] %s\n", ok ? "ALL PASS" : "FAILED");
  return ok ? 0 : 1;
}
