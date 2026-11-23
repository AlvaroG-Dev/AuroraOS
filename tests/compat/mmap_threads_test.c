#define _GNU_SOURCE
// mmap_threads_test: varios hilos haciendo mmap/munmap/mprotect/mremap a la vez
// sobre el mismo proceso y tocando la memoria (fallos de demanda). Wine lo
// hace al arrancar (wineboot crea hilos que reservan pilas mientras ntdll
// mapea DLLs). Sin serializar el espacio de direcciones, la lista de VMAs se
// corrompia y aparecian fallos "not in any VMA" sobre memoria recien mapeada.
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_32BIT
#define MAP_32BIT 0x40
#endif

#define NTHREADS 8
#define ITERS 3000

static volatile int fallos;

static void *worker(void *arg) {
  unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
  for (int i = 0; i < ITERS; i++) {
    seed = seed * 1103515245u + 12345u;
    size_t pages = 1 + (seed >> 16) % 16;
    size_t len = pages * 4096;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | ((seed & 8) ? MAP_32BIT : 0);
    unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) {
      __atomic_add_fetch(&fallos, 1, __ATOMIC_RELAXED);
      continue;
    }
    for (size_t o = 0; o < len; o += 4096)
      p[o] = (unsigned char)(o >> 12) ^ (unsigned char)i;
    for (size_t o = 0; o < len; o += 4096)
      if (p[o] != ((unsigned char)(o >> 12) ^ (unsigned char)i))
        __atomic_add_fetch(&fallos, 1, __ATOMIC_RELAXED);
    if ((seed & 6) == 6) {
      unsigned char *q = mremap(p, len, len * 2, MREMAP_MAYMOVE);
      if (q != MAP_FAILED) {
        if (q[0] != ((unsigned char)0 ^ (unsigned char)i))
          __atomic_add_fetch(&fallos, 1, __ATOMIC_RELAXED);
        q[len] = 1;
        p = q;
        len *= 2;
      }
    }
    if (seed & 1)
      mprotect(p, 4096, PROT_READ);
    munmap(p, len);
  }
  return NULL;
}

int main(void) {
  pthread_t t[NTHREADS];
  for (long i = 0; i < NTHREADS; i++)
    if (pthread_create(&t[i], NULL, worker, (void *)(i + 1))) {
      printf("MMAP-THREADS: FALLO pthread_create\n");
      return 1;
    }
  for (int i = 0; i < NTHREADS; i++)
    pthread_join(t[i], NULL);
  printf("MMAP-THREADS: %s (%d hilos x %d iteraciones, %d errores)\n",
         fallos ? "FALLO" : "OK", NTHREADS, ITERS, fallos);
  return fallos != 0;
}
