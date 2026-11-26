// pi_mutex_test: mutex PTHREAD_PRIO_INHERIT (futex LOCK_PI/UNLOCK_PI) con
// varios hilos compitiendo, trylock y timedlock. Antes AuroraOS devolvia
// -ENOSYS a FUTEX_UNLOCK_PI y los hilos que esperaban no despertaban nunca.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define NT 6
#define IT 20000
static pthread_mutex_t m;
static volatile long cuenta;
static volatile int tomado;

static void *w(void *a) {
  (void)a;
  for (int i = 0; i < IT; i++) {
    pthread_mutex_lock(&m);
    long c = cuenta;
    if ((i & 255) == 0) sched_yield();
    cuenta = c + 1;
    pthread_mutex_unlock(&m);
  }
  return NULL;
}

static void *holder(void *a) {
  (void)a;
  pthread_mutex_lock(&m);
  tomado = 1;
  usleep(300000);
  pthread_mutex_unlock(&m);
  return NULL;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  pthread_mutexattr_t at;
  pthread_mutexattr_init(&at);
  pthread_mutexattr_setprotocol(&at, PTHREAD_PRIO_INHERIT);
  pthread_mutex_init(&m, &at);
  int bad = 0;
  pthread_t t[NT];
  for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, w, NULL);
  for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
  if (cuenta != (long)NT * IT) { printf("  FALLO cuenta %ld\n", cuenta); bad++; }
  pthread_t h;
  pthread_create(&h, NULL, holder, NULL);
  while (!tomado) sched_yield();
  if (pthread_mutex_trylock(&m) != EBUSY) { printf("  FALLO trylock\n"); bad++; }
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
  if (pthread_mutex_timedlock(&m, &ts) != ETIMEDOUT) { printf("  FALLO timedlock corto\n"); bad++; }
  clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
  if (pthread_mutex_timedlock(&m, &ts) != 0) { printf("  FALLO timedlock largo\n"); bad++; }
  else pthread_mutex_unlock(&m);
  pthread_join(h, NULL);
  printf("PI-MUTEX: %s (%d hilos x %d, cuenta %ld)\n", bad ? "FALLO" : "OK", NT, IT, cuenta);
  return bad != 0;
}
