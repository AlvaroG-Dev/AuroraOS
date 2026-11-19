// drm_master_test: abrir /dev/dri/card0 NO da el maestro DRM (el escritorio
// lo pinta el compositor del kernel); DRM_IOCTL_SET_MASTER si, y al soltarlo
// o cerrar, el compositor vuelve a presentar.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOCTL_SET_MASTER _IO(DRM_IOCTL_BASE, 0x1e)
#define DRM_IOCTL_DROP_MASTER _IO(DRM_IOCTL_BASE, 0x1f)
#define DRM_IOCTL_AUTH_MAGIC _IOW(DRM_IOCTL_BASE, 0x11, unsigned int)

static int ok, bad;
#define CHECK(c, m) do { if (c) { ok++; printf("  ok  %s\n", m); } else { bad++; printf("  FALLO %s (errno %d)\n", m, errno); } } while (0)

static int is_master(int fd) { // como drmIsMaster de libdrm
  unsigned int magic = 0;
  return ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &magic) == 0 || errno != EACCES;
}

int main(void) {
  int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    printf("DRM-MASTER-TEST: sin /dev/dri/card0 (%s), omitido\n", strerror(errno));
    return 0;
  }
  CHECK(!is_master(fd), "abrir card0 no da el maestro");
  printf("DRM-MASTER-TEST: fd abierto sin maestro, el compositor debe seguir presentando\n");
  fflush(stdout);
  sleep(8);
  int fd2 = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  CHECK(fd2 >= 0 && !is_master(fd2), "segundo open tampoco");
  CHECK(ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0, "SET_MASTER explicito");
  CHECK(is_master(fd), "ya es maestro");
  CHECK(ioctl(fd2, DRM_IOCTL_SET_MASTER, 0) != 0, "otro fd no puede quitarselo");
  CHECK(ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) == 0, "DROP_MASTER");
  CHECK(!is_master(fd), "ya no es maestro");
  CHECK(ioctl(fd2, DRM_IOCTL_SET_MASTER, 0) == 0, "el otro fd lo toma");
  close(fd2); // cerrar suelta el maestro
  int fd3 = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  CHECK(fd3 >= 0 && !is_master(fd3), "tras cerrar, nadie es maestro");
  close(fd3);
  close(fd);
  printf("DRM-MASTER-TEST: %d ok, %d fallos%s\n", ok, bad, bad ? "" : " ALL PASS");
  return bad != 0;
}
