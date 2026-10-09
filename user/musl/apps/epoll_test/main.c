#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>


int efd, ep;

void *writer(void *a) {
  (void)a;
  usleep(200000); // 200 ms
  uint64_t v = 1;
  write(efd, &v, 8); // despierta el epoll
  return NULL;
}

int main() {
  ep = epoll_create1(0);
  efd = eventfd(0, 0);

  struct epoll_event ev = {0};
  ev.events = EPOLLIN;
  ev.data.u64 = 0xABCD; // ← acceder al campo u64 de la union
  epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);

  pthread_t t;
  pthread_create(&t, NULL, writer, NULL);

  struct epoll_event out[4];
  int n = epoll_wait(ep, out, 4, 5000);
  printf("epoll_wait -> %d, data=0x%llx\n", n,
         (unsigned long long)out[0].data.u64); // ← aquí también .u64

  uint64_t v;
  read(efd, &v, 8);
  pthread_join(t, NULL);
  return 0;
}