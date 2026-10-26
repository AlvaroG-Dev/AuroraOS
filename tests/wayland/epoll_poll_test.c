// tests/wayland/epoll_poll_test.c
// poll()/select() sobre un fd de epoll: debe ser legible solo cuando hay
// eventos pendientes (GLib lo usa con /proc/self/mountinfo; si siempre se da
// por listo el bucle principal gira sin dormir).
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("[epoll_poll_test] FAIL %s\n", msg); fails++; } else printf("[epoll_poll_test] ok   %s\n", msg); } while (0)

static int ready(int epfd) {
  struct pollfd p = {epfd, POLLIN, 0};
  return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

int main(void) {
  int ep = epoll_create1(EPOLL_CLOEXEC);
  int ev = eventfd(0, EFD_NONBLOCK);
  struct epoll_event e = {.events = EPOLLIN, .data.fd = ev};
  epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e);
  CHECK(!ready(ep), "epoll vacio: poll no esta listo");
  uint64_t one = 1;
  write(ev, &one, 8);
  CHECK(ready(ep), "con el eventfd escrito: poll listo");
  struct epoll_event out[2];
  CHECK(epoll_wait(ep, out, 2, 0) == 1, "epoll_wait devuelve 1 evento");
  uint64_t v;
  read(ev, &v, 8);
  CHECK(!ready(ep), "tras leer el eventfd: poll no esta listo");

  // Fichero normal con EPOLLET (como GLib con /proc/self/mountinfo): se
  // informa una vez y deja de estar listo.
  int ep2 = epoll_create1(EPOLL_CLOEXEC);
  int fd = open("/proc/self/mountinfo", O_RDONLY);
  if (fd < 0)
    fd = open("/proc/self/status", O_RDONLY);
  struct epoll_event e2 = {.events = EPOLLIN | EPOLLET, .data.fd = fd};
  epoll_ctl(ep2, EPOLL_CTL_ADD, fd, &e2);
  CHECK(ready(ep2), "EPOLLET: primera vez listo");
  CHECK(epoll_wait(ep2, out, 2, 0) == 1, "EPOLLET: epoll_wait lo consume");
  CHECK(!ready(ep2), "EPOLLET: despues ya no esta listo (sin giro)");
  CHECK(epoll_wait(ep2, out, 2, 0) == 0, "EPOLLET: epoll_wait vacio");

  // poll con timeout sobre el epoll vacio duerme (no vuelve de inmediato).
  struct pollfd p = {ep, POLLIN, 0};
  int n = poll(&p, 1, 150);
  CHECK(n == 0, "poll con timeout 150 ms sobre epoll vacio vence sin eventos");
  printf(fails ? "EPOLLPOLL-FAIL\n" : "EPOLLPOLL-OK\n");
  return fails != 0;
}
