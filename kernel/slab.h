// kernel/slab.h
#ifndef KERNEL_SLAB_H
#define KERNEL_SLAB_H

#include "memlayout.h"
#include "spinlock.h"
#include <stddef.h>
#include <stdint.h>

#define SLAB_MAX_SIZE 2048
#define SLAB_NUM_CACHES 8

extern const size_t slab_sizes[SLAB_NUM_CACHES];

typedef struct slab_header slab_header_t;

typedef struct slab_cache {
  spinlock_t lock;
  size_t obj_size;
  size_t objs_per_slab;

  slab_header_t *slabs;       // slabs con al menos 1 objeto en uso
  slab_header_t *empty_slabs; // slabs vacíos, listos para reuso rápido

  size_t slabs_count; // entradas en c->slabs
  size_t empty_count; // entradas en c->empty_slabs
  size_t used_count;  // objetos en uso
  size_t total_count; // objetos en c->slabs (activos)

  const char *name;
} slab_cache_t;

void slab_init(void);
void *slab_alloc(size_t size);
void slab_free(void *ptr);

int slab_is_slab_ptr(const void *ptr);
size_t slab_usable_size(const void *ptr);

// [HARDENING] Límites del rango virtual del SLAB.
uint64_t slab_vma_start(void);
uint64_t slab_vma_end(void);

// [NUEVO] Devuelve páginas al PMM hasta liberar ~bytes.
// Devuelve los bytes realmente liberados.
size_t slab_reclaim_pages(size_t target_bytes);

void slab_dump_stats(void);

#endif