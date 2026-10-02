// user/musl/apps/pthread_test/main.c
//
// Test de pthread_create + futex + clone(CLONE_VM|CLONE_THREAD).
// Debe imprimir tids distintos, el worker debe retornar 43, y el
// join debe completarse sin cuelgues.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static long gettid_(void) { return syscall(SYS_gettid); }
static long getpid_(void) { return syscall(SYS_getpid); }

// ---------------------------------------------------------------------------
// Test 1: un solo thread con valor de retorno.
// ---------------------------------------------------------------------------
static void *worker(void *arg) {
  long v = (long)arg;
  printf("worker: arg=%ld tid=%ld pid=%ld\n", v, gettid_(), getpid_());
  return (void *)(v + 1);
}

static int test_basic(void) {
  pthread_t t;
  void *ret = NULL;

  printf("[1] basic: main tid=%ld pid=%ld\n", gettid_(), getpid_());
  int rc = pthread_create(&t, NULL, worker, (void *)42);
  if (rc != 0) {
    printf("    FAIL: pthread_create rc=%d\n", rc);
    return 1;
  }
  rc = pthread_join(t, &ret);
  if (rc != 0) {
    printf("    FAIL: pthread_join rc=%d\n", rc);
    return 1;
  }
  if ((long)ret != 43) {
    printf("    FAIL: ret=%ld esperado 43\n", (long)ret);
    return 1;
  }
  printf("    OK: ret=%ld\n", (long)ret);
  return 0;
}

// ---------------------------------------------------------------------------
// Test 2: varios threads en paralelo, todos deben correr.
// ---------------------------------------------------------------------------
#define N 8

struct slot {
  long id;
  long result;
};

static void *worker_n(void *arg) {
  struct slot *s = (struct slot *)arg;
  s->result = s->id * 100 + gettid_();
  return NULL;
}

static int test_many(void) {
  pthread_t ts[N];
  struct slot slots[N];

  printf("[2] many: lanzando %d threads\n", N);
  for (long i = 0; i < N; i++) {
    slots[i].id = i;
    slots[i].result = -1;
    int rc = pthread_create(&ts[i], NULL, worker_n, &slots[i]);
    if (rc != 0) {
      printf("    FAIL: create[%ld] rc=%d\n", i, rc);
      return 1;
    }
  }
  for (int i = 0; i < N; i++) {
    int rc = pthread_join(ts[i], NULL);
    if (rc != 0) {
      printf("    FAIL: join[%d] rc=%d\n", i, rc);
      return 1;
    }
    if (slots[i].result < 0) {
      printf("    FAIL: slot[%d] no escrito\n", i);
      return 1;
    }
  }
  printf("    OK: %d threads completados\n", N);
  return 0;
}

// ---------------------------------------------------------------------------
// Test 3: mutex + condición (usa futex de verdad, no solo clear_child_tid).
// ---------------------------------------------------------------------------
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int shared_flag = 0;

static void *waiter(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mtx);
  while (!shared_flag)
    pthread_cond_wait(&cv, &mtx);
  pthread_mutex_unlock(&mtx);
  printf("    waiter: despertado\n");
  return NULL;
}

static int test_mutex_cond(void) {
  pthread_t t;
  printf("[3] mutex+cond\n");

  int rc = pthread_create(&t, NULL, waiter, NULL);
  if (rc != 0) {
    printf("    FAIL: create rc=%d\n", rc);
    return 1;
  }

  usleep(100000); // 100 ms

  pthread_mutex_lock(&mtx);
  shared_flag = 1;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mtx);

  pthread_join(t, NULL);
  printf("    OK\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Test 4: stress — muchos ciclos de create/join.
// ---------------------------------------------------------------------------
#define STRESS_ITERS 50

static void *noop(void *arg) { return arg; }

static int test_stress(void) {
  printf("[4] stress: %d iteraciones\n", STRESS_ITERS);
  for (int i = 0; i < STRESS_ITERS; i++) {
    pthread_t t;
    void *ret = NULL;
    int rc = pthread_create(&t, NULL, noop, (void *)(long)i);
    if (rc != 0) {
      printf("    FAIL: iter=%d create rc=%d\n", i, rc);
      return 1;
    }
    rc = pthread_join(t, &ret);
    if (rc != 0 || (long)ret != i) {
      printf("    FAIL: iter=%d join rc=%d ret=%ld\n", i, rc, (long)ret);
      return 1;
    }
  }
  printf("    OK\n");
  return 0;
}

int main(void) {
  printf("=== pthread_test ===\n");
  int fails = 0;
  fails += test_basic();
  fails += test_many();
  fails += test_mutex_cond();
  fails += test_stress();
  printf("=== %s ===\n", fails ? "FALLOS" : "TODO OK");
  return fails;
}