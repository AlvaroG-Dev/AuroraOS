#pragma once
#include <stdint.h>

// Inicializa el buddy allocator con el tamaño total (en páginas) y el bitmap interno
void buddy_init(uint64_t total_pages, uint8_t *bitmap_ptr);

// Asigna un bloque de orden "order" (2^order páginas). Devuelve dirección física o 0
uint64_t buddy_alloc_order(uint32_t order);

// Libera un bloque de orden "order" en la dirección física dada
void buddy_free_order(uint64_t phys_addr, uint32_t order);

// helper: convertir número de páginas a orden (si es potencia de dos), devuelve -1 si no lo es
int32_t pages_to_order(uint64_t pages);
