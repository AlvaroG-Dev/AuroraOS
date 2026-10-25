// evdev_test: /dev/input/event0 (teclado) y event1 (raton) con la ABI evdev
// de Linux: ioctls EVIOC*, struct input_event y poll. Si se le pasa "wait",
// espera hasta 25 s a recibir una pulsacion de 'a' y un movimiento de raton
// (los inyecta scripts/qemu-boot-test.sh con QMON="sendkey a;mouse_move ...").
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct ev {
  int64_t sec, usec;
  uint16_t type, code;
  int32_t value;
};

#define EVIOCGVERSION _IOR('E', 0x01, int)
#define EVIOCGID _IOR('E', 0x02, uint16_t[4])
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGBIT(e, len) _IOC(_IOC_READ, 'E', 0x20 + (e), len)
#define EVIOCGRAB _IOW('E', 0x90, int)

static int fails;
#define CHECK(c, name)                                                         \
  do {                                                                         \
    if (c)                                                                     \
      printf("[evdev_test] PASS %s\n", name);                                  \
    else {                                                                     \
      printf("[evdev_test] FAIL %s (errno=%d)\n", name, errno);                \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static int testbit(const unsigned char *m, int b) { return (m[b / 8] >> (b % 8)) & 1; }

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int wait_events = argc > 1 && !strcmp(argv[1], "wait");

  int kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  int mfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  CHECK(kfd >= 0 && mfd >= 0, "open event0/event1");
  if (kfd < 0 || mfd < 0)
    return 1;

  int ver = 0;
  CHECK(ioctl(kfd, EVIOCGVERSION, &ver) == 0 && ver == 0x010001, "EVIOCGVERSION");
  char name[64] = {0};
  CHECK(ioctl(kfd, EVIOCGNAME(sizeof(name)), name) > 0 && name[0], "EVIOCGNAME");
  printf("[evdev_test]   event0 = '%s'\n", name);
  uint16_t id[4] = {0};
  CHECK(ioctl(mfd, EVIOCGID, id) == 0 && id[1] != 0, "EVIOCGID");

  unsigned char ev[4] = {0}, keys[96] = {0}, rel[2] = {0};
  CHECK(ioctl(kfd, EVIOCGBIT(0, sizeof(ev)), ev) >= 0 && testbit(ev, 1) &&
            !testbit(ev, 2),
        "teclado: EV_KEY sin EV_REL");
  CHECK(ioctl(kfd, EVIOCGBIT(1, sizeof(keys)), keys) >= 0 && testbit(keys, 30) &&
            testbit(keys, 1) && testbit(keys, 57),
        "teclado: KEY_A/ESC/SPACE");
  memset(ev, 0, sizeof(ev));
  memset(keys, 0, sizeof(keys));
  CHECK(ioctl(mfd, EVIOCGBIT(0, sizeof(ev)), ev) >= 0 && testbit(ev, 2),
        "raton: EV_REL");
  CHECK(ioctl(mfd, EVIOCGBIT(1, sizeof(keys)), keys) >= 0 && testbit(keys, 0x110),
        "raton: BTN_LEFT");
  CHECK(ioctl(mfd, EVIOCGBIT(2, sizeof(rel)), rel) >= 0 && testbit(rel, 0) &&
            testbit(rel, 1),
        "raton: REL_X/REL_Y");

  struct ev e;
  CHECK(read(kfd, &e, sizeof(e)) < 0 && errno == EAGAIN, "read sin datos -> EAGAIN");
  struct pollfd pf[2] = {{kfd, POLLIN, 0}, {mfd, POLLIN, 0}};
  CHECK(poll(pf, 2, 0) == 0, "poll sin datos");

  if (wait_events) {
    printf("[evdev_test] READY\n");
    int got_key = 0, got_up = 0, got_rel = 0;
    time_t t0 = time(NULL);
    while (time(NULL) - t0 < 25 && !(got_key && got_up && got_rel)) {
      pf[0].revents = pf[1].revents = 0;
      if (poll(pf, 2, 500) <= 0)
        continue;
      struct ev b[16];
      for (int i = 0; i < 2; i++) {
        if (!(pf[i].revents & POLLIN))
          continue;
        ssize_t n = read(i ? mfd : kfd, b, sizeof(b));
        for (ssize_t k = 0; k < n / (ssize_t)sizeof(struct ev); k++) {
          if (i == 0 && b[k].type == 1 && b[k].code == 30) {
            if (b[k].value == 1)
              got_key = 1;
            if (b[k].value == 0)
              got_up = 1;
          }
          if (i == 1 && b[k].type == 2 && b[k].code <= 1)
            got_rel = 1;
        }
      }
    }
    CHECK(got_key, "evento KEY_A down (QEMU sendkey)");
    CHECK(got_up, "evento KEY_A up");
    CHECK(got_rel, "evento REL_X/REL_Y (QEMU mouse_move)");
  }

  CHECK(ioctl(kfd, EVIOCGRAB, 1) == 0, "EVIOCGRAB 1");
  int kfd2 = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
  CHECK(kfd2 >= 0 && ioctl(kfd2, EVIOCGRAB, 1) < 0 && errno == EBUSY,
        "segundo grab -> EBUSY");
  CHECK(ioctl(kfd, EVIOCGRAB, 0) == 0, "EVIOCGRAB 0");

  // VT / consola: lo que libseat y los compositores hacen al arrancar.
  {
    int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    CHECK(tty >= 0, "open /dev/tty0");
    if (tty >= 0) {
      int mode = -1;
      CHECK(ioctl(tty, 0x4B3B /*KDGETMODE*/, &mode) == 0 && mode == 0, "KDGETMODE = KD_TEXT");
      CHECK(ioctl(tty, 0x4B3A /*KDSETMODE*/, 1) == 0, "KDSETMODE KD_GRAPHICS");
      CHECK(ioctl(tty, 0x4B3B, &mode) == 0 && mode == 1, "KDGETMODE = KD_GRAPHICS");
      CHECK(ioctl(tty, 0x4B3A, 0) == 0, "KDSETMODE KD_TEXT");
      struct { unsigned short a, s, st; } vs;
      CHECK(ioctl(tty, 0x5603 /*VT_GETSTATE*/, &vs) == 0 && vs.a == 1, "VT_GETSTATE activo=1");
      struct { char m, w; short r, a, f; } vm = {1, 0, 10, 12, 0};
      CHECK(ioctl(tty, 0x5602 /*VT_SETMODE*/, &vm) == 0, "VT_SETMODE");
      CHECK(ioctl(tty, 0x5606 /*VT_ACTIVATE*/, 1) == 0 && ioctl(tty, 0x5607 /*VT_WAITACTIVE*/, 1) == 0,
            "VT_ACTIVATE/WAITACTIVE");
      close(tty);
    }
  }

  printf("[evdev_test] %s (%d fallos)\n", fails ? "FAILED" : "ALL PASS", fails);
  return fails ? 1 : 0;
}
