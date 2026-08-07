// kernel/heap.c
// Kernel Heap: Implicit Free List allocator (First-Fit + Coalescing)

#include "heap.h"
#include "paging.h"
#include "serial.h"

// ---------------------------------------------------------------------------
// Cabecera de cada bloque del heap
// ---------------------------------------------------------------------------
typedef struct block_header {
  size_t size; // Tamanio del payload (sin la cabecera)
  int is_free;
  struct block_header *next; // Siguiente bloque en la lista
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)
#define HEAP_GROW                                                              \
  4 // Numero de paginas a pedir al VMM cuando el heap se queda sin espacio

static block_header_t *heap_head = NULL; // Primer bloque
static uint64_t heap_top = 0;            // Proxima direccion virtual libre

// ---------------------------------------------------------------------------
// Internas
// ---------------------------------------------------------------------------

// Expande el heap pidiendo 'pages' paginas al VMM y crea un bloque libre.
static block_header_t *heap_grow(size_t pages) {
  uint64_t vaddr = heap_top;
  if (vmm_alloc_pages(vaddr, pages, PTE_WRITABLE) != 0) {
    serial_puts("[HEAP] ERROR: vmm_alloc_pages fallo\n");
    return NULL;
  }
  heap_top += pages * PAGE_SIZE;

  block_header_t *blk = (block_header_t *)vaddr;
  blk->size = pages * PAGE_SIZE - HEADER_SIZE;
  blk->is_free = 1;
  blk->next = NULL;

  // Enlazar al final de la lista
  if (!heap_head) {
    heap_head = blk;
  } else {
    block_header_t *cur = heap_head;
    while (cur->next)
      cur = cur->next;
    cur->next = blk;
  }
  return blk;
}

// Fusiona bloques libres adyacentes (coalescing)
static void coalesce(void) {
  block_header_t *cur = heap_head;
  while (cur && cur->next) {
    if (cur->is_free && cur->next->is_free) {
      // Absorber el siguiente bloque
      cur->size += HEADER_SIZE + cur->next->size;
      cur->next = cur->next->next;
    } else {
      cur = cur->next;
    }
  }
}

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------

void heap_init(void) {
  heap_top = HEAP_VMA;
  heap_head = NULL;
  // Reservar un par de paginas iniciales para tener espacio de sobra
  heap_grow(HEAP_GROW);
  serial_puts("[HEAP] Heap inicializado en 0x");
  serial_hex(HEAP_VMA);
  serial_puts("\n");
}

void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;
  // Alinear a 16 bytes para cumplir con el ABI System V (SSE)
  size = (size + 15) & ~(size_t)15;

  // Buscar primer bloque libre suficientemente grande (First-Fit)
  block_header_t *cur = heap_head;
  while (cur) {
    if (cur->is_free && cur->size >= size) {
      // Si el bloque es mucho mas grande, dividirlo
      if (cur->size >= size + HEADER_SIZE + 16) {
        block_header_t *split =
            (block_header_t *)((uint8_t *)cur + HEADER_SIZE + size);
        split->size = cur->size - size - HEADER_SIZE;
        split->is_free = 1;
        split->next = cur->next;
        cur->size = size;
        cur->next = split;
      }
      cur->is_free = 0;
      return (void *)((uint8_t *)cur + HEADER_SIZE);
    }
    cur = cur->next;
  }

  // No hay bloques libres: expandir el heap
  size_t pages_needed = (size + HEADER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages_needed < HEAP_GROW)
    pages_needed = HEAP_GROW;
  block_header_t *new_blk = heap_grow(pages_needed);
  if (!new_blk)
    return NULL;

  // Dividir si es necesario
  if (new_blk->size >= size + HEADER_SIZE + 16) {
    block_header_t *split =
        (block_header_t *)((uint8_t *)new_blk + HEADER_SIZE + size);
    split->size = new_blk->size - size - HEADER_SIZE;
    split->is_free = 1;
    split->next = new_blk->next;
    new_blk->size = size;
    new_blk->next = split;
  }
  new_blk->is_free = 0;
  return (void *)((uint8_t *)new_blk + HEADER_SIZE);
}

void kfree(void *ptr) {
  if (!ptr)
    return;
  block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
  blk->is_free = 1;
  coalesce();
}

void heap_dump(void) {
  serial_puts("[HEAP] Estado del heap:\n");
  block_header_t *cur = heap_head;
  int idx = 0;
  while (cur) {
    serial_puts("  [");
    serial_putn(idx++, 10, 0);
    serial_puts("] addr=0x");
    serial_hex((uint64_t)cur);
    serial_puts(" size=");
    serial_putn(cur->size, 10, 0);
    serial_puts(cur->is_free ? " FREE\n" : " USED\n");
    cur = cur->next;
  }
}