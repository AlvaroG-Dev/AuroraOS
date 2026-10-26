// tests/wayland/fuzz_test.c
//
// [Bloque 7.1] Pruebas de estres/fuzzing de la capa Linux que usa Wayland:
//   1. ioctls DRM aleatorios sobre /dev/dri/card0 (numeros, tamanos y punteros
//      al azar, validos e invalidos): el kernel no debe caer ni colgarse.
//   2. AF_UNIX: socketpair/stream con SCM_RIGHTS aleatorios, cierres a mitad
//      de mensaje, fds invalidos, truncado de cmsg.
//   3. Cierre de fd con buffers: abrir/cerrar card0 y crear/destruir dumb
//      buffers y framebuffers sin liberarlos (el cierre debe limpiar).
// Termina imprimiendo "FUZZ-OK" si el proceso sobrevive a todo.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

#define DRM_IOCTL_BASE 'd'
static unsigned long drm_ioc(unsigned nr, unsigned size, unsigned dir) {
  return ((unsigned long)dir << 30) | ((unsigned long)size << 16) |
         ((unsigned long)DRM_IOCTL_BASE << 8) | nr;
}

static void fuzz_drm(int iters) {
  int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    printf("[fuzz] sin /dev/dri/card0 (%s): se omite DRM\n", strerror(errno));
    return;
  }
  uint8_t *page = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int ok = 0, err = 0;
  for (int i = 0; i < iters; i++) {
    unsigned nr = rnd() % 0xD0;
    unsigned size = (unsigned)(rnd() % 256);
    unsigned dir = (unsigned)(rnd() & 3);
    for (unsigned k = 0; k < 512; k++)
      page[k] = (uint8_t)rnd();
    // A veces campos tipicos pequenos para pasar la validacion inicial.
    if (rnd() & 1)
      memset(page, 0, 64);
    unsigned long arg;
    switch (rnd() % 4) {
    case 0: arg = (unsigned long)page; break;
    case 1: arg = 0; break;
    case 2: arg = (unsigned long)(rnd() & 0xFFFFFFFFFFFFull); break;
    default: arg = (unsigned long)page + (rnd() % 4096); break;
    }
    int rc = ioctl(fd, drm_ioc(nr, size, dir), arg);
    if (rc == 0) ok++; else err++;
    if (i % 997 == 0) { // de vez en cuando se reabre: limpieza de master/fbs/blobs
      close(fd);
      fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
      if (fd < 0)
        break;
    }
  }
  if (fd >= 0)
    close(fd);
  munmap(page, 1 << 16);
  printf("[fuzz] DRM: %d ioctls ok, %d con error, el kernel sigue vivo\n", ok, err);
}

static void fuzz_dumb_lifecycle(int rounds) {
  struct create { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
  struct mapd { uint32_t handle, pad; uint64_t offset; };
  int total = 0;
  for (int r = 0; r < rounds; r++) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
      return;
    for (int i = 0; i < 6; i++) {
      struct create c = {.height = 64 + (rnd() % 256), .width = 64 + (rnd() % 256), .bpp = 32};
      if (ioctl(fd, drm_ioc(0xB2, sizeof(c), 3), &c) == 0) {
        total++;
        struct mapd m = {.handle = c.handle};
        if (ioctl(fd, drm_ioc(0xB3, sizeof(m), 3), &m) == 0) {
          void *p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
          if (p != MAP_FAILED) {
            memset(p, 0x5A, c.size);
            if (rnd() & 1)
              munmap(p, c.size);
          }
        }
        // sin DESTROY_DUMB: el cierre del fd debe liberar todo
      }
    }
    close(fd);
  }
  printf("[fuzz] DRM dumb: %d buffers creados y abandonados al cerrar\n", total);
}

static void fuzz_unix(int rounds) {
  int made = 0;
  for (int r = 0; r < rounds; r++) {
    int sv[2];
    if (socketpair(AF_UNIX, (rnd() & 1) ? SOCK_STREAM : SOCK_SEQPACKET, 0, sv) < 0)
      continue;
    made++;
    for (int k = 0; k < 8; k++) {
      uint8_t buf[256];
      size_t n = rnd() % sizeof(buf);
      for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)rnd();
      struct iovec iov = {buf, n ? n : 1};
      union { struct cmsghdr h; char c[CMSG_SPACE(sizeof(int) * 4)]; } u;
      memset(&u, 0, sizeof(u));
      struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
      int nfds = (int)(rnd() % 5);
      if (nfds) {
        mh.msg_control = u.c;
        mh.msg_controllen = (rnd() & 1) ? sizeof(u.c) : CMSG_LEN(sizeof(int) * (size_t)nfds);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = (rnd() % 8) ? SCM_RIGHTS : 99; // a veces tipo invalido
        cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        for (int i = 0; i < nfds; i++) {
          int fdv = (rnd() % 4 == 0) ? (int)(rnd() % 1000) /* fd invalido */
                                     : ((rnd() & 1) ? sv[0] : 1);
          memcpy(CMSG_DATA(cm) + sizeof(int) * (size_t)i, &fdv, sizeof(int));
        }
      }
      (void)sendmsg(sv[0], &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
      // Recibir con un buffer de control demasiado pequeno a veces.
      char cbuf[64];
      struct iovec riov = {buf, sizeof(buf)};
      struct msghdr rm = {.msg_iov = &riov, .msg_iovlen = 1,
                          .msg_control = cbuf,
                          .msg_controllen = (rnd() & 1) ? sizeof(cbuf) : 0};
      ssize_t got = recvmsg(sv[1], &rm, MSG_DONTWAIT);
      if (got > 0 && rm.msg_controllen) {
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&rm); cm; cm = CMSG_NXTHDR(&rm, cm))
          if (cm->cmsg_type == SCM_RIGHTS) {
            int nr = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < nr; i++) {
              int fdv;
              memcpy(&fdv, CMSG_DATA(cm) + sizeof(int) * (size_t)i, sizeof(int));
              close(fdv);
            }
          }
      }
      if (rnd() % 5 == 0) { // cierre a mitad de comunicacion
        close(sv[rnd() & 1]);
        break;
      }
    }
    close(sv[0]);
    close(sv[1]);
  }
  printf("[fuzz] AF_UNIX: %d pares de sockets, sin caidas\n", made);
}

// Hijos que mueren (SIGKILL) con un socket y un memfd abiertos: el lado que
// queda no debe colgarse (EOF/EPIPE).
static void fuzz_dead_peers(int rounds) {
  int ok = 0;
  for (int r = 0; r < rounds; r++) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
      continue;
    pid_t pid = fork();
    if (pid == 0) {
      close(sv[0]);
      int m = memfd_create("fz", 0);
      if (m >= 0) {
        ftruncate(m, 1 << 16);
        mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0);
      }
      write(sv[1], "hola", 4);
      for (;;)
        pause();
    }
    close(sv[1]);
    char b[8];
    struct pollfd p = {sv[0], POLLIN, 0};
    poll(&p, 1, 2000);
    (void)!read(sv[0], b, sizeof(b));
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    struct pollfd p2 = {sv[0], POLLIN | POLLHUP, 0};
    int pr = poll(&p2, 1, 1000);
    ssize_t n = read(sv[0], b, sizeof(b));
    if (pr >= 0 && n == 0)
      ok++;
    close(sv[0]);
  }
  printf("[fuzz] pares muertos: %d/%d con EOF correcto\n", ok, rounds);
}

int main(int argc, char **argv) {
  int scale = argc > 1 ? atoi(argv[1]) : 1;
  if (scale < 1) scale = 1;
  fuzz_drm(4000 * scale);
  fuzz_dumb_lifecycle(30 * scale);
  fuzz_unix(300 * scale);
  fuzz_dead_peers(12 * scale);
  printf("FUZZ-OK\n");
  return 0;
}
