// kernel/buddy.c
#include "buddy.h"
#include "pmm.h"
#include "serial.h"
#include "klog.h"
#include <stddef.h>
#include "spinlock.h"

static uint64_t buddy_total_pages = 0;
static uint8_t *buddy_bitmap = NULL; // apuntamos al mismo bitmap de pmm
static uint32_t buddy_max_order = 0;
static spinlock_t buddy_lock;

static uint32_t floor_log2(uint64_t v) {
    uint32_t r = 0;
    while (v >>= 1) r++;
    return r;
}

void buddy_init(uint64_t total_pages, uint8_t *bitmap_ptr) {
    buddy_total_pages = total_pages;
    buddy_bitmap = bitmap_ptr;
    buddy_max_order = floor_log2(total_pages);
    spin_init(&buddy_lock);
    LOG_INFO("[BUDDY] Inicializado. max_order=%u", buddy_max_order);
}

// Comprueba si un rango de bits [start, start+count) está libre (0)
static int range_free(uint64_t start, uint64_t count) {
    if (!buddy_bitmap) return 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t b = start + i;
        if (b >= buddy_total_pages) return 0;
        if (buddy_bitmap[b / 8] & (1 << (b % 8))) return 0;
    }
    return 1;
}

// Marca un rango como usado (1)
static void range_mark_used(uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t b = start + i;
        buddy_bitmap[b / 8] |= (1 << (b % 8));
    }
}

// Marca un rango como libre (0)
static void range_mark_free(uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t b = start + i;
        buddy_bitmap[b / 8] &= ~(1 << (b % 8));
    }
}

// Buscar un bloque de orden 'order' o de mayor orden y hacer splits implícitos
uint64_t buddy_alloc_order(uint32_t order) {
    if (order > buddy_max_order) return 0;
    unsigned long flags = spin_lock_irqsave(&buddy_lock);

    for (uint32_t o = order; o <= buddy_max_order; o++) {
        uint64_t size = (1ULL << o);
        for (uint64_t start = 0; start + size <= buddy_total_pages; start += size) {
            if (range_free(start, size)) {
                // Si encontramos un bloque de mayor orden, vamos a dividirlo mentalmente
                // y asignar la primera mitad repetidamente hasta alcanzar 'order'.
                uint64_t cur_start = start;
                uint32_t cur_order = o;
                while (cur_order > order) {
                    cur_order--;
                    // mantener primera mitad como candidata
                    // la segunda mitad permanece libre (ya lo está)
                    // cur_start stays the same
                }
                // Marcar la región como usada (2^order páginas)
                uint64_t final_size = (1ULL << order);
                range_mark_used(cur_start, final_size);
                spin_unlock_irqrestore(&buddy_lock, flags);
                return cur_start * PAGE_SIZE;
            }
        }
    }

    spin_unlock_irqrestore(&buddy_lock, flags);
    return 0;
}

// Liberar con coalescing: intentar fusionar con el buddy si está libre
void buddy_free_order(uint64_t phys_addr, uint32_t order) {
    if (order > buddy_max_order) return;
    uint64_t start = phys_addr / PAGE_SIZE;
    unsigned long flags = spin_lock_irqsave(&buddy_lock);

    uint64_t cur_start = start;
    uint32_t cur_order = order;

    // Intentar fusionar hacia arriba
    while (cur_order < buddy_max_order) {
        uint64_t buddy = cur_start ^ (1ULL << cur_order);
        uint64_t buddy_index = buddy; // index in pages
        if (buddy_index + (1ULL << cur_order) > buddy_total_pages) break;

        if (!range_free(buddy_index, (1ULL << cur_order))) break; // buddy no está libre

        // Buddy está libre => fusionar
        if (buddy_index < cur_start) cur_start = buddy_index;
        cur_order++;
    }

    // Marcar la región fusionada como libre
    range_mark_free(cur_start, (1ULL << cur_order));
    spin_unlock_irqrestore(&buddy_lock, flags);
}

int32_t pages_to_order(uint64_t pages) {
    if (pages == 0) return -1;
    if ((pages & (pages - 1)) != 0) return -1;
    int32_t order = 0;
    while (pages > 1) { pages >>= 1; order++; }
    return order;
}
