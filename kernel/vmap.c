// kernel/vmap.c — ver vmap.h.
//
// Asignador por extensiones: una lista ordenada de huecos libres y otra de
// áreas en uso (dirección y tamaño, para vmap_area_free). Las reservas son
// pocas y grandes (buffers de drivers), así que la búsqueda lineal basta.
#include "vmap.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "test.h"
#include "spinlock.h"

typedef struct vmap_ext {
  uint64_t start, size; // size incluye la página de guarda
  struct vmap_ext *next;
} vmap_ext_t;

static vmap_ext_t *g_free, *g_used;
static spinlock_t g_vmap_lock;
static uint64_t g_used_bytes;

void vmap_init(void) {
  spin_init(&g_vmap_lock);
  // Crear ya el PDPT de VMAP_BASE (mapear y quitar una página): a partir de
  // aquí cada proceso nuevo hereda la entrada del PML4.
  uint64_t phys = pmm_alloc_page();
  if (!phys || paging_map_page(VMAP_BASE, phys, PTE_PRESENT | PTE_NX) != 0 ||
      paging_unmap_page(VMAP_BASE) != 0)
    LOG_ERR("[VMAP] no se pudo crear el PDPT de la región");
  if (phys)
    pmm_free_page(phys);
  g_free = (vmap_ext_t *)kzalloc(sizeof(vmap_ext_t));
  if (!g_free) {
    LOG_ERR("[VMAP] sin memoria");
    return;
  }
  g_free->start = VMAP_BASE;
  g_free->size = VMAP_SIZE;
}

uint64_t vmap_area_alloc(uint64_t size) {
  if (!size || size > VMAP_SIZE)
    return 0;
  size = ((size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1)) + PAGE_SIZE;
  vmap_ext_t *u = (vmap_ext_t *)kzalloc(sizeof(*u));
  if (!u)
    return 0;
  unsigned long fl = spin_lock_irqsave(&g_vmap_lock);
  vmap_ext_t **pp = &g_free;
  while (*pp && (*pp)->size < size)
    pp = &(*pp)->next;
  if (!*pp) {
    spin_unlock_irqrestore(&g_vmap_lock, fl);
    kfree(u);
    return 0;
  }
  vmap_ext_t *f = *pp;
  u->start = f->start;
  u->size = size;
  f->start += size;
  f->size -= size;
  vmap_ext_t *gone = NULL;
  if (!f->size) {
    *pp = f->next;
    gone = f;
  }
  u->next = g_used;
  g_used = u;
  g_used_bytes += size - PAGE_SIZE;
  spin_unlock_irqrestore(&g_vmap_lock, fl);
  kfree(gone);
  return u->start;
}

int vmap_area_map(uint64_t va, uint64_t phys, uint64_t flags) {
  if (va < VMAP_BASE || va >= VMAP_BASE + VMAP_SIZE || (va & 0xFFF))
    return -1;
  flags &= ~(uint64_t)PTE_USER;
  return paging_map_page(va, phys & PTE_FRAME, flags | PTE_PRESENT);
}

void vmap_area_free(uint64_t va) {
  unsigned long fl = spin_lock_irqsave(&g_vmap_lock);
  vmap_ext_t **pp = &g_used;
  while (*pp && (*pp)->start != va)
    pp = &(*pp)->next;
  vmap_ext_t *u = *pp;
  if (u) {
    *pp = u->next;
    g_used_bytes -= u->size - PAGE_SIZE;
  }
  spin_unlock_irqrestore(&g_vmap_lock, fl);
  if (!u) {
    LOG_ERR("[VMAP] vmap_area_free de %p desconocida", (void *)va);
    return;
  }
  for (uint64_t p = u->start; p < u->start + u->size; p += PAGE_SIZE)
    paging_unmap_page(p);
  paging_invalidate_tlb_global(0); // otras CPU pueden tener la traducción

  // Devolver el hueco a la lista libre (ordenada) y juntarlo con vecinos.
  fl = spin_lock_irqsave(&g_vmap_lock);
  vmap_ext_t **fp = &g_free, *prev = NULL;
  while (*fp && (*fp)->start < u->start) {
    prev = *fp;
    fp = &(*fp)->next;
  }
  vmap_ext_t *next = *fp, *drop = NULL;
  if (prev && prev->start + prev->size == u->start) {
    prev->size += u->size;
    drop = u;
    u = prev;
  } else {
    u->next = next;
    *fp = u;
  }
  vmap_ext_t *drop2 = NULL;
  if (next && u->start + u->size == next->start) {
    u->size += next->size;
    u->next = next->next;
    drop2 = next;
  }
  spin_unlock_irqrestore(&g_vmap_lock, fl);
  kfree(drop);
  kfree(drop2);
}

uint64_t vmap_area_used(void) { return g_used_bytes; }

// Dos páginas sueltas (no contiguas en física) se ven seguidas en la región;
// la página de guarda queda sin mapear y el hueco se reutiliza al liberar.
static void vmap_selftest(void) {
  uint64_t used0 = vmap_area_used();
  uint64_t p0 = pmm_alloc_page(), p1 = pmm_alloc_page();
  uint64_t va = vmap_area_alloc(2 * PAGE_SIZE);
  TEST_ASSERT(p0 && p1 && va >= VMAP_BASE, "vmap: reserva en la región");
  if (!p0 || !p1 || !va)
    return;
  TEST_ASSERT(vmap_area_map(va, p1, PTE_WRITABLE | PTE_NX) == 0 &&
                  vmap_area_map(va + PAGE_SIZE, p0, PTE_WRITABLE | PTE_NX) == 0,
              "vmap: mapear dos páginas");
  memset((void *)va, 0xA5, 2 * PAGE_SIZE);
  TEST_ASSERT(((uint8_t *)phys_to_virt(p1))[0] == 0xA5 &&
                  ((uint8_t *)phys_to_virt(p0))[PAGE_SIZE - 1] == 0xA5,
              "vmap: escritura visible por la ventana física");
  TEST_ASSERT(paging_get_phys(va + 2 * PAGE_SIZE) == 0, "vmap: página de guarda sin mapear");
  TEST_ASSERT(vmap_area_used() == used0 + 2 * PAGE_SIZE, "vmap: contabilidad");
  vmap_area_free(va);
  TEST_ASSERT(paging_get_phys(va) == 0 && vmap_area_used() == used0, "vmap: liberar desmapea");
  uint64_t va2 = vmap_area_alloc(PAGE_SIZE);
  TEST_ASSERT(va2 == va, "vmap: el hueco se reutiliza");
  if (va2)
    vmap_area_free(va2);
  pmm_free_page(p0);
  pmm_free_page(p1);
}

REGISTER_TEST_FLAGS("vmap: páginas sueltas en la región del kernel", vmap_selftest,
                    TEST_FLAG_BLOCKING);
