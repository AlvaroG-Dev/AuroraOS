// kernel/heap.c
// Kernel Heap: Implicit Free List allocator (First-Fit + Coalescing)
// Thread-safe con spinlock (IRQ-safe).
//
// Este archivo sirve los bloques > SLAB_MAX_SIZE. Los bloques pequeños
// van al SLAB (ver slab.c). kmalloc/kfree deciden a qué allocator ir
// según el tamaño y la dirección.

#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "panic.h"
#include "serial.h"
#include "slab.h"
#include "spinlock.h"
#include "string.h"

// ---------------------------------------------------------------------------
// Cabecera de cada bloque del heap
// ---------------------------------------------------------------------------
typedef struct __attribute__((aligned(16))) block_header {
  size_t size;               // offset 0
  uint32_t is_free;          // offset 8
  uint32_t magic;            // offset 12
  struct block_header *next; // offset 16
  uint64_t _pad;             // offset 24  → total 32, alineado a 16
} block_header_t;

#define HEAP_MAGIC 0xCAFEBABE
#define HEADER_SIZE sizeof(block_header_t) // == 32
#define HEAP_GROW 4 // páginas a pedir al VMM cuando se agota el heap

// Límite máximo por una sola allocación. Evita que un kmalloc con
// un tamaño absurdo (basura, typo, corrupción) intente reservar
// cientos de MB y agote el PMM. 64 MB es holgado para todo uso
// razonable del kernel actual; si en el futuro hace falta más,
// subir el límite aquí.
#define HEAP_MAX_SINGLE_ALLOC (64 * 1024 * 1024)

static block_header_t *heap_head = NULL; // Primer bloque
static uint64_t heap_top = 0;            // Próxima dirección virtual libre
static spinlock_t heap_lock;             // Protege heap_head / heap_top

// ---------------------------------------------------------------------------
// Internas (sin lock — el llamante debe tener heap_lock cogido)
// ---------------------------------------------------------------------------

static block_header_t *heap_grow_locked(size_t pages) {
  // [FIX] Rechazar peticiones que excedan el límite. Nadie debería
  // pedir más de HEAP_MAX_SINGLE_ALLOC de una vez.
  size_t max_pages = HEAP_MAX_SINGLE_ALLOC / PAGE_SIZE;
  if (pages > max_pages) {
    LOG_ERR("[HEAP] petición de %lu páginas excede el límite (%lu)",
            (unsigned long)pages, (unsigned long)max_pages);
    return NULL;
  }

  uint64_t vaddr = heap_top;
  if (vmm_alloc_pages(vaddr, pages, PTE_WRITABLE | PTE_NX) != 0) {
    LOG_ERR("[HEAP] ERROR: vmm_alloc_pages fallo");
    return NULL;
  }
  heap_top += pages * PAGE_SIZE;

  block_header_t *blk = (block_header_t *)vaddr;
  blk->size = pages * PAGE_SIZE - HEADER_SIZE;
  blk->is_free = 1;
  blk->magic = HEAP_MAGIC;
  blk->next = NULL;

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

static void coalesce(void) {
  block_header_t *cur = heap_head;
  while (cur && cur->next) {
    if (cur->is_free && cur->next->is_free) {
      cur->size += HEADER_SIZE + cur->next->size;
      cur->next = cur->next->next;
    } else {
      cur = cur->next;
    }
  }
}

static void *kmalloc_locked(size_t size) {
  // [FIX] Rechazar peticiones absurdas antes de tocar el PMM.
  if (size > HEAP_MAX_SINGLE_ALLOC) {
    LOG_ERR("[HEAP] kmalloc(%lu) excede el límite de %lu bytes",
            (unsigned long)size, (unsigned long)HEAP_MAX_SINGLE_ALLOC);
    return NULL;
  }

  block_header_t *cur = heap_head;
  while (cur) {
    if (cur->is_free && cur->size >= size) {
      if (cur->size >= size + HEADER_SIZE + 16) {
        block_header_t *split =
            (block_header_t *)((uint8_t *)cur + HEADER_SIZE + size);
        split->size = cur->size - size - HEADER_SIZE;
        split->is_free = 1;
        split->magic = HEAP_MAGIC;
        split->next = cur->next;
        cur->size = size;
        cur->next = split;
      }
      cur->is_free = 0;
      cur->magic = HEAP_MAGIC;
      return (void *)((uint8_t *)cur + HEADER_SIZE);
    }
    cur = cur->next;
  }

  size_t pages_needed = (size + HEADER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages_needed < HEAP_GROW)
    pages_needed = HEAP_GROW;

  block_header_t *new_blk = heap_grow_locked(pages_needed);
  if (!new_blk)
    return NULL;

  if (new_blk->size >= size + HEADER_SIZE + 16) {
    block_header_t *split =
        (block_header_t *)((uint8_t *)new_blk + HEADER_SIZE + size);
    split->size = new_blk->size - size - HEADER_SIZE;
    split->is_free = 1;
    split->magic = HEAP_MAGIC;
    split->next = new_blk->next;
    new_blk->size = size;
    new_blk->next = split;
  }
  new_blk->is_free = 0;
  new_blk->magic = HEAP_MAGIC;

  return (void *)((uint8_t *)new_blk + HEADER_SIZE);
}

static void kfree_locked(void *ptr) {
  if (!ptr)
    return;

  uint64_t p = (uint64_t)ptr;
  if (p < HEAP_VMA || p >= heap_top) {
    LOG_ERR("[HEAP] kfree: puntero fuera del heap: %p (heap=[%p, %p))", ptr,
            (void *)HEAP_VMA, (void *)heap_top);
    return;
  }

  block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);

  if (blk->magic != HEAP_MAGIC) {
    LOG_ERR("[HEAP] kfree invalido: ptr=%p blk=%p magic=%p", ptr, (void *)blk,
            (void *)(uint64_t)blk->magic);
    return;
  }

  blk->is_free = 1;
  coalesce();
}

// ---------------------------------------------------------------------------
// Helper: tamaño usable de un bloque del heap.
// Debe llamarse con heap_lock cogido.
// Devuelve 0 si ptr no es un bloque válido del heap.
// ---------------------------------------------------------------------------
static size_t heap_usable_size_locked(void *ptr) {
  if (!ptr)
    return 0;
  uint64_t p = (uint64_t)ptr;
  if (p < HEAP_VMA || p >= heap_top)
    return 0;
  block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
  if (blk->magic != HEAP_MAGIC)
    return 0;
  return blk->size;
}

// ---------------------------------------------------------------------------
// Helper: tamaño usable real de un bloque (SLAB o heap).
// Usado por kzalloc y krealloc.
// ---------------------------------------------------------------------------
static size_t usable_size_any(void *ptr) {
  if (!ptr)
    return 0;

  if (slab_is_slab_ptr(ptr)) {
    return slab_usable_size(ptr);
  }

  unsigned long flags = spin_lock_irqsave(&heap_lock);
  size_t s = heap_usable_size_locked(ptr);
  spin_unlock_irqrestore(&heap_lock, flags);
  return s;
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

void heap_init(void) {
  spin_init(&heap_lock);

  unsigned long flags = spin_lock_irqsave(&heap_lock);
  heap_top = HEAP_VMA;
  heap_head = NULL;
  heap_grow_locked(HEAP_GROW);
  spin_unlock_irqrestore(&heap_lock, flags);

  LOG_INFO("[HEAP] Heap inicializado en %p (límite por alloc: %lu MB)",
           (void *)HEAP_VMA,
           (unsigned long)(HEAP_MAX_SINGLE_ALLOC / (1024 * 1024)));

  // Inicializar SLAB después del heap. SLAB no depende del heap para
  // crecer (pide páginas al VMM directamente), pero el orden es natural:
  // heap primero, slab después.
  slab_init();
}

void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;

  // Camino rápido: objetos pequeños → SLAB.
  if (size <= SLAB_MAX_SIZE) {
    return slab_alloc(size);
  }

  // Camino slow: first-fit con alineación a 16 bytes.
  size = (size + 15) & ~(size_t)15;

  unsigned long flags = spin_lock_irqsave(&heap_lock);
  void *result = kmalloc_locked(size);
  spin_unlock_irqrestore(&heap_lock, flags);
  return result;
}

void kfree(void *ptr) {
  if (!ptr)
    return;

  // Camino rápido: SLAB.
  if (slab_is_slab_ptr(ptr)) {
    slab_free(ptr);
    return;
  }

  unsigned long flags = spin_lock_irqsave(&heap_lock);
  kfree_locked(ptr);
  spin_unlock_irqrestore(&heap_lock, flags);
}

void heap_dump(void) {
  // [PANIC] Si estamos en panic, NO coger el lock. Si el panic ocurrió
  // dentro de kmalloc, heap_lock puede estar cogido y nos colgaríamos
  // intentando cogerlo otra vez.
  extern int panic_in_progress(void);
  int locked = !panic_in_progress();
  unsigned long flags = 0;

  if (locked)
    flags = spin_lock_irqsave(&heap_lock);

  LOG_INFO("[HEAP] Estado del heap (bloques > %d bytes):", SLAB_MAX_SIZE);
  block_header_t *cur = heap_head;
  int idx = 0;
  while (cur) {
    LOG_INFO("  [%u] addr=%p size=%lu%s magic=%p", idx++, (void *)cur,
             (unsigned long)cur->size, cur->is_free ? " FREE" : " USED",
             (void *)(uint64_t)cur->magic);
    cur = cur->next;
  }

  if (locked)
    spin_unlock_irqrestore(&heap_lock, flags);

  // NO llamar a slab_dump_stats() aquí. El SLAB tiene su propio dump
  // (dump_slab / slab_dump_stats) y ya se invoca por separado desde
  // panic.c. Evitamos así la duplicación.
}

// ---------------------------------------------------------------------------
// Wrappers estilo kernel
// ---------------------------------------------------------------------------

void *kzalloc(size_t size) {
  if (size == 0)
    return NULL;
  void *p = kmalloc(size);
  if (!p)
    return NULL;
  // Pedimos el tamaño real del bloque (redondeado al cache SLAB o al
  // bloque del heap) para no dejar bytes sin inicializar.
  size_t usable = usable_size_any(p);
  memset(p, 0, usable);
  return p;
}

void *kcalloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0)
    return NULL;

  // Overflow check: nmemb * size no debe desbordar size_t.
  if (nmemb > (size_t)-1 / size)
    return NULL;

  size_t total = nmemb * size;
  return kzalloc(total);
}

void *krealloc(void *ptr, size_t new_size) {
  // Casos borde.
  if (ptr == NULL)
    return kmalloc(new_size);
  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  size_t old_usable = usable_size_any(ptr);
  if (old_usable == 0) {
    LOG_ERR("[HEAP] krealloc: puntero inválido %p", ptr);
    return NULL;
  }

  // Caso 1: el bloque actual es SLAB y el nuevo tamaño todavía cabe en
  // el mismo cache. No movemos.
  if (slab_is_slab_ptr(ptr) && new_size <= SLAB_MAX_SIZE) {
    size_t current_cache_size = slab_usable_size(ptr);
    if (new_size <= current_cache_size)
      return ptr;
  }

  // Caso 2: el bloque actual es del heap y ya es suficientemente grande.
  // No movemos.
  if (!slab_is_slab_ptr(ptr) && old_usable >= new_size)
    return ptr;

  // Caso general: alloc nuevo, copiar, liberar viejo.
  void *new_ptr = kmalloc(new_size);
  if (!new_ptr)
    return NULL; // el bloque viejo sigue siendo válido

  size_t to_copy = (old_usable < new_size) ? old_usable : new_size;
  memcpy(new_ptr, ptr, to_copy);
  kfree(ptr);
  return new_ptr;
}

char *kstrdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  if (len == (size_t)-1)
    return NULL;
  char *copy = (char *)kmalloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, s, len);
  copy[len] = '\0';
  return copy;
}

char *kstrndup(const char *s, size_t n) {
  if (!s)
    return NULL;
  // Buscar el NUL dentro de los primeros n bytes.
  size_t len = 0;
  while (len < n && s[len] != '\0')
    len++;
  char *copy = (char *)kmalloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, s, len);
  copy[len] = '\0';
  return copy;
}