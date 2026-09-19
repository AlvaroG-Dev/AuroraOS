#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Buddy allocator sobre bitmap plano.
//
// Comparte el bitmap con el PMM. Es el MISMO bitmap que pmm.c usa para
// llevar el estado de cada página física (1 bit por página: 0 = libre,
// 1 = usada).
//
// CONTRATO DE LA API:
//
//   - buddy_* NO tiene lock propio. El llamante DEBE tener el lock del
//     PMM (pmm_lock) cogido antes de llamar. Esto es así porque el bitmap
//     es compartido con pmm.c y ambos escriben sobre él. Si buddy_*
//     cogiera su propio lock, habría race con pmm_alloc_page/free_page
//     (que también escriben sobre el bitmap bajo pmm_lock).
//
//   - buddy_free_order(addr, order) SOLO se puede llamar con bloques
//     que se obtuvieron de buddy_alloc_order(order). Liberar una parte
//     de un bloque, o un bloque con orden distinto al asignado, corrompe
//     el bitmap.
//
//   - La dirección devuelta por buddy_alloc_order está alineada a
//     2^order páginas.
//
// En el código actual, los únicos llamantes son pmm_alloc_pages,
// pmm_free_pages y pmm_free_page, que ya tienen pmm_lock cogido. Si en
// el futuro se añaden más llamantes, deben hacer lo mismo.
// ---------------------------------------------------------------------------

// Inicializa el buddy allocator. El bitmap debe ser el mismo que usa pmm.c.
// Llamar UNA VEZ durante el boot, antes de que haya concurrencia.
// (En la práctica, desde pmm_relocate_bitmap, que corre en el BSP con
// las interrupciones deshabilitadas.)
void buddy_init(uint64_t total_pages, uint8_t *bitmap_ptr);

// Asigna un bloque de 2^order páginas. Devuelve dirección FÍSICA o 0.
// REQUIERE: pmm_lock cogido por el llamante.
uint64_t buddy_alloc_order(uint32_t order);

// Libera un bloque de 2^order páginas en la dirección física dada.
// Aplica coalescing: si el buddy está libre, fusiona recursivamente.
// REQUIERE: pmm_lock cogido por el llamante.
// REQUIERE: el bloque debe haber sido asignado con buddy_alloc_order(order).
void buddy_free_order(uint64_t phys_addr, uint32_t order);

// Convierte un número de páginas a orden si es potencia de 2.
// Devuelve -1 si no lo es, 0 para 1 página, 1 para 2, etc.
int32_t pages_to_order(uint64_t pages);
