// tests/wayland/futex_test.c
// FUTEX_WAKE_OP / FUTEX_CMP_REQUEUE (los usa glibc/libstdc++: sin ellos Qt y
// GLib se quedaban dormidos para siempre tras recibir -ENOSYS).
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("[futex_test] FAIL %s\n", msg); fails++; } else printf("[futex_test] ok   %s\n", msg); } while (0)

static long fx(uint32_t *a, int op, uint32_t v, long v2, uint32_t *b, uint32_t v3) {
  return syscall(SYS_futex, a, op | FUTEX_PRIVATE_FLAG, v, v2, b, v3);
}
#define OP(op, oparg, cmp, cmparg) \
  ((uint32_t)(((op) << 28) | ((cmp) << 24) | (((oparg) & 0xfff) << 12) | ((cmparg) & 0xfff)))

static uint32_t A, B, C;
static void *waiter(void *arg) {
  uint32_t *w = arg;
  while (__atomic_load_n(w, __ATOMIC_ACQUIRE) == 0)
    fx(w, FUTEX_WAIT, 0, 0, NULL, 0);
  return NULL;
}

int main(void) {
  // Efecto sobre *uaddr2 sin esperadores.
  uint32_t v = 0;
  CHECK(fx(&A, FUTEX_WAKE_OP, 1, 1, &v, OP(0, 5, 0, 0)) == 0 && v == 5, "SET 5");
  fx(&A, FUTEX_WAKE_OP, 1, 1, &v, OP(1, 3, 0, 0));
  CHECK(v == 8, "ADD 3");
  fx(&A, FUTEX_WAKE_OP, 1, 1, &v, OP(2, 0x10, 0, 0));
  CHECK(v == 0x18, "OR 0x10");
  fx(&A, FUTEX_WAKE_OP, 1, 1, &v, OP(3, 0x08, 0, 0));
  CHECK(v == 0x10, "ANDN 0x08 (borra bits)");
  fx(&A, FUTEX_WAKE_OP, 1, 1, &v, OP(4, 0x11, 0, 0));
  CHECK(v == 0x01, "XOR 0x11");
  fx(&A, FUTEX_WAKE_OP, 1, 1, &v, OP(8 | 0, 4, 0, 0));
  CHECK(v == 16, "SET con OPARG_SHIFT (1<<4)");

  // Despierta en A y, con cmp cumplido, tambien en B.
  pthread_t ta, tb;
  A = 0; B = 0;
  pthread_create(&ta, NULL, waiter, &A);
  pthread_create(&tb, NULL, waiter, &B);
  usleep(200000);
  __atomic_store_n(&A, 1, __ATOMIC_RELEASE);
  // B: old == 0 (EQ 0) -> se cumple -> despierta tambien a B; B = 1.
  long r = fx(&A, FUTEX_WAKE_OP, 1, 1, &B, OP(0, 1, 0, 0));
  printf("[futex_test] WAKE_OP devolvio %ld\n", r);
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);
  CHECK(B == 1, "WAKE_OP desperto A y B (cmp cumplido)");

  // CMP_REQUEUE: valor distinto -> EAGAIN; igual -> despierta.
  C = 0;
  CHECK(fx(&C, FUTEX_CMP_REQUEUE, 1, 1, &B, 99) == -1 && errno == EAGAIN, "CMP_REQUEUE con valor distinto = EAGAIN");
  pthread_t tc;
  pthread_create(&tc, NULL, waiter, &C);
  usleep(200000);
  __atomic_store_n(&C, 1, __ATOMIC_RELEASE);
  long rq = fx(&C, FUTEX_CMP_REQUEUE, 1, 1, &B, 1);
  printf("[futex_test] CMP_REQUEUE devolvio %ld\n", rq);
  pthread_join(tc, NULL);
  CHECK(rq >= 0, "CMP_REQUEUE despierta al esperador");
  printf(fails ? "FUTEX-FAIL\n" : "FUTEX-OK\n");
  return fails != 0;
}
