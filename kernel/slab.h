// kernel/slab.h
#ifndef KERNEL_SLAB_H
#define KERNEL_SLAB_H

#include "spinlock.h"
#include <stddef.h>
#include <stdint.h>

// SLAB allocator para objetos pequeños. Caches por tamaño:
// 16, 32, 64, 128, 256, 512, 1024, 2048 bytes.
//
// Los objetos de tamaño <= SLAB_MAX_SIZE se sirven desde aquí.
// Los más grandes van al heap first-fit (heap.c).
//
// Región virtual: SLAB_VMA .. slab_top. Separada del heap para que
// slab_is_slab_ptr() sea trivial (comparación de rango) y no haya
// falsos positivos con punteros del heap.

#define SLAB_VMA 0xFFFFFFFF83000000ULL
#define SLAB_MAX_SIZE 2048
#define SLAB_NUM_CACHES 8

// Tamaños de objeto soportados. Deben estar en orden ascendente.
extern const size_t slab_sizes[SLAB_NUM_CACHES];

typedef struct slab_header slab_header_t;

typedef struct slab_cache {
  spinlock_t lock;      // protege slabs, freelist, contadores
  size_t obj_size;      // tamaño del objeto (redondeado a 8)
  size_t objs_per_slab; // objetos por slab (>=1)
  slab_header_t *slabs; // lista enlazada de slabs del cache
  size_t slabs_count;   // número de slabs en la lista
  size_t used_count;    // objetos actualmente en uso
  size_t total_count;   // objetos asignados en slabs
  const char *name;     // para debug
} slab_cache_t;

// Inicializa todas las caches. Debe llamarse DESPUÉS de heap_init.
void slab_init(void);

// Alloc/free para objetos pequeños. Llamados por kmalloc/kfree.
// Devuelven NULL si size == 0 o size > SLAB_MAX_SIZE.
void *slab_alloc(size_t size);
void slab_free(void *ptr);

// ¿Es 'ptr' un puntero servido por SLAB?
// (usa comparación de rango, no mira magic ni cabeceras)
int slab_is_slab_ptr(const void *ptr);

// Tamaño usable del objeto (útil para krealloc). 0 si ptr no es SLAB.
size_t slab_usable_size(const void *ptr);

// Debug
void slab_dump_stats(void);

#endif