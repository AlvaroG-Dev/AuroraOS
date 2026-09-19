// kernel/buddy.c
//
// Buddy allocator sobre bitmap plano, compartido con pmm.c.
//
// IMPORTANTE: este archivo NO tiene lock propio. Todas las funciones
// que tocan el bitmap asumen que el llamante tiene pmm_lock cogido.
// Ver buddy.h para el contrato completo.
//
// La razón: el bitmap es el mismo que usa pmm.c para pmm_alloc_page,
// pmm_free_page, etc. Si buddy_* cogiera su propio lock, habría race
// con esas funciones (ambos escriben sobre el mismo byte).

#include "buddy.h"
#include "pmm.h"
#include "serial.h"
#include "klog.h"
#include <stddef.h>

static uint64_t buddy_total_pages = 0;
static uint8_t *buddy_bitmap = NULL; // apuntamos al mismo bitmap de pmm
static uint32_t buddy_max_order = 0;

// floor_log2: mayor k tal que 2^k <= v. v debe ser > 0.
static uint32_t floor_log2(uint64_t v) {
    uint32_t r = 0;
    while (v >>= 1) r++;
    return r;
}

void buddy_init(uint64_t total_pages, uint8_t *bitmap_ptr) {
    if (total_pages == 0 || !bitmap_ptr) {
        LOG_ERR("[BUDDY] init inválido: total_pages=%lu bitmap=%p",
                (unsigned long)total_pages, (void *)bitmap_ptr);
        buddy_total_pages = 0;
        buddy_bitmap = NULL;
        buddy_max_order = 0;
        return;
    }
    buddy_total_pages = total_pages;
    buddy_bitmap = bitmap_ptr;
    // Mayor orden posible. Si total_pages no es potencia de 2, el mayor
    // bloque asignable es 2^floor_log2(total_pages) páginas.
    buddy_max_order = floor_log2(total_pages);
    LOG_INFO("[BUDDY] Inicializado. total_pages=%lu max_order=%u",
             (unsigned long)total_pages, buddy_max_order);
}

// ---------------------------------------------------------------------------
// Helpers sobre el bitmap. NO tienen lock; el llamante debe tener pmm_lock.
// ---------------------------------------------------------------------------

// Comprueba si un rango de bits [start, start+count) está libre (0).
// Devuelve 1 si todo el rango está libre y dentro de los límites.
static int range_free(uint64_t start, uint64_t count) {
    if (!buddy_bitmap) return 0;
    if (start + count > buddy_total_pages) return 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t b = start + i;
        if (buddy_bitmap[b / 8] & (1 << (b % 8))) return 0;
    }
    return 1;
}

// Marca un rango como usado (1).
static void range_mark_used(uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t b = start + i;
        buddy_bitmap[b / 8] |= (1 << (b % 8));
    }
}

// Marca un rango como libre (0).
static void range_mark_free(uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t b = start + i;
        buddy_bitmap[b / 8] &= ~(1 << (b % 8));
    }
}

// ---------------------------------------------------------------------------
// API pública. REQUIERE: pmm_lock cogido por el llamante.
// ---------------------------------------------------------------------------

uint64_t buddy_alloc_order(uint32_t order) {
    if (!buddy_bitmap || order > buddy_max_order) return 0;

    // Buscar el bloque más pequeño (menor orden) que pueda satisfacer la
    // petición. Empezamos en `order` y subimos.
    for (uint32_t o = order; o <= buddy_max_order; o++) {
        uint64_t size = (1ULL << o);
        // Los bloques de orden `o` están alineados a 2^o páginas.
        for (uint64_t start = 0; start + size <= buddy_total_pages;
             start += size) {
            if (range_free(start, size)) {
                // Encontrado. Asignamos la primera mitad (2^order páginas)
                // desde `start`. El resto del bloque queda libre, y el
                // coalescing posterior lo tratará como bloques libres.
                //
                // Ejemplo: pedimos orden 1 (2 páginas), encontramos un
                // bloque de orden 3 (8 páginas) en [0,7]. Asignamos [0,1].
                // [2,3], [4,7] quedan libres. Si luego se libera [0,1],
                // buddy_free_order fusionará con [2,3] (buddy de orden 1)
                // y luego con [4,7] (buddy de orden 2), recomponiendo
                // el bloque original si nadie más lo ha tocado.
                uint64_t final_size = (1ULL << order);
                range_mark_used(start, final_size);
                return start * PAGE_SIZE;
            }
        }
    }
    return 0;
}

void buddy_free_order(uint64_t phys_addr, uint32_t order) {
    if (!buddy_bitmap || order > buddy_max_order) return;
    if (phys_addr % PAGE_SIZE != 0) {
        LOG_ERR("[BUDDY] free_order: dirección no alineada a página: 0x%lx",
                (unsigned long)phys_addr);
        return;
    }

    uint64_t start = phys_addr / PAGE_SIZE;
    if (start + (1ULL << order) > buddy_total_pages) {
        LOG_ERR("[BUDDY] free_order: rango fuera de límites: start=%lu "
                "order=%u total=%lu",
                (unsigned long)start, order, (unsigned long)buddy_total_pages);
        return;
    }

    uint64_t cur_start = start;
    uint32_t cur_order = order;

    // Coalescing: subir mientras el buddy esté libre.
    while (cur_order < buddy_max_order) {
        uint64_t buddy = cur_start ^ (1ULL << cur_order);
        // El buddy debe estar dentro de los límites.
        if (buddy + (1ULL << cur_order) > buddy_total_pages) break;
        // Y debe estar completamente libre.
        if (!range_free(buddy, (1ULL << cur_order))) break;

        // Fusionar: el bloque fusionado empieza en el menor de los dos.
        if (buddy < cur_start) cur_start = buddy;
        cur_order++;
    }

    // Marcar el bloque fusionado como libre.
    range_mark_free(cur_start, (1ULL << cur_order));
}

int32_t pages_to_order(uint64_t pages) {
    if (pages == 0) return -1;
    if ((pages & (pages - 1)) != 0) return -1;
    // pages es potencia de 2. El orden es log2(pages).
    // __builtin_ctzll(pages) cuenta los ceros a la derecha, que es
    // exactamente log2(pages) para potencias de 2.
    return (int32_t)__builtin_ctzll(pages);
}
