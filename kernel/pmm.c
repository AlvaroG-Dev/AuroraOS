// kernel/pmm.c
#include "pmm.h"
#include "buddy.h"
#include "klog.h"
#include "paging.h"
#include "sched.h" // sched_get_ticks
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include <stddef.h>

extern uint8_t _kernel_end; // definido en linker script

static uint8_t *bitmap = 0;
static uint64_t bitmap_phys_global = 0;
static uint64_t max_blocks = 0;
static uint64_t bitmap_size_bytes = 0;
static uint64_t used_blocks = 0;
static uint64_t last_alloc_bit = 0;
static uint64_t total_usable_pages = 0;

// [PF-SLOW] Rangos EFI_CONVENTIONAL_MEMORY capturados en pmm_init().
// Sirven para que el pre-touch de map_phys_window() no toque MMIO.
// 64 entradas es holgado: un EFI memory map típico tiene <30.
#define PMM_MAX_USABLE_RANGES 64

typedef struct {
  uint64_t start; // dirección física, inclusiva
  uint64_t end;   // dirección física, exclusiva
} pmm_usable_range_t;

static pmm_usable_range_t usable_ranges[PMM_MAX_USABLE_RANGES];
static int usable_ranges_count = 0;

#define EFI_CONVENTIONAL_MEMORY 7

static int pmm_validate_efi_range(uint64_t phys, uint64_t pages,
                                  uint64_t *size_out) {
  if (pages > UINT64_MAX / PAGE_SIZE)
    return 0;

  uint64_t size = pages * PAGE_SIZE;
  if (phys > UINT64_MAX - size)
    return 0;

  if (size_out)
    *size_out = size;
  return 1;
}

static inline int bitmap_in_range(uint64_t bit) { return bit < max_blocks; }

static inline int bitmap_test_safe(uint8_t *bmp, uint64_t bit) {
  if (!bmp || !bitmap_in_range(bit))
    return 1;
  return !!(bmp[bit / 8] & (1 << (bit % 8)));
}
static inline void bitmap_set_safe(uint8_t *bmp, uint64_t bit) {
  if (!bmp || !bitmap_in_range(bit))
    return;
  bmp[bit / 8] |= (1 << (bit % 8));
}
static inline void bitmap_clear_safe(uint8_t *bmp, uint64_t bit) {
  if (!bmp || !bitmap_in_range(bit))
    return;
  bmp[bit / 8] &= ~(1 << (bit % 8));
}

static spinlock_t pmm_lock;

// ---------------------------------------------------------------------------
// [COW] Refcount por frame. Guarda las referencias EXTRA de cada frame (0 =
// un solo dueño, que es lo normal), así un array a cero significa "nada
// compartido". Vive en memoria obtenida de pmm_alloc_pages tras buddy_init;
// si no se pudo reservar, pmm_page_share() devuelve -1 y fork copia.
//
// pmm_free_page() solo libera el frame cuando no quedan referencias extra,
// de modo que munmap/exit/brk/madvise funcionan sin cambios con páginas
// compartidas por fork.
// ---------------------------------------------------------------------------
static uint16_t *page_extra = 0;

void pmm_init(uint64_t memmap, uint64_t memmap_size,
              uint64_t memmap_desc_size) {
  if (!memmap || memmap_size == 0)
    return;
  // EFI_MEMORY_DESCRIPTOR has a 40-byte minimum layout. Validate the
  // stride before any loop; a zero/short stride could otherwise cause an
  // infinite loop or out-of-bounds reads on a corrupt boot handoff.
  if (memmap_desc_size < 40 || memmap_desc_size > memmap_size ||
      memmap_size % memmap_desc_size != 0) {
    LOG_ERR("[PMM] mapa EFI inválido: size=%lu desc_size=%lu",
            (unsigned long)memmap_size, (unsigned long)memmap_desc_size);
    return;
  }

  spin_init(&pmm_lock);

  uint8_t *ptr = (uint8_t *)memmap;
  uint64_t max_phys_addr = 0;

  for (uint64_t i = 0; i < memmap_size; i += memmap_desc_size) {
    uint32_t type = *(uint32_t *)(ptr + i + 0);
    if (type != EFI_CONVENTIONAL_MEMORY)
      continue;
    uint64_t phys = *(uint64_t *)(ptr + i + 8);
    uint64_t pages = *(uint64_t *)(ptr + i + 24);
    uint64_t size;
    if (!pmm_validate_efi_range(phys, pages, &size))
      continue;
    uint64_t end_addr = phys + size;
    if (end_addr > max_phys_addr) {
      max_phys_addr = end_addr;
    }
  }

  if (max_phys_addr == 0) {
    LOG_ERR("[PMM] ERROR: mapa de memoria invalido (no hay memoria usable)");
    return;
  }

  max_blocks = max_phys_addr / PAGE_SIZE;
  bitmap_size_bytes = (max_blocks + 7) / 8;

  uint64_t bitmap_phys = 0;
  int bitmap_found = 0;
  for (uint64_t i = 0; i < memmap_size; i += memmap_desc_size) {
    uint32_t type = *(uint32_t *)(ptr + i + 0);
    uint64_t phys = *(uint64_t *)(ptr + i + 8);
    uint64_t pages = *(uint64_t *)(ptr + i + 24);

    /*
     * This is a second pass over the EFI map. Do not assume descriptors
     * validated by the first pass are still safe here: malformed entries
     * must never wrap pages * PAGE_SIZE or phys + bitmap_size_bytes.
     */
    if (type != EFI_CONVENTIONAL_MEMORY || phys == 0)
      continue;
    uint64_t size;
    if (!pmm_validate_efi_range(phys, pages, &size))
      continue;
    if (size < bitmap_size_bytes)
      continue;
    if (phys > UINT64_MAX - bitmap_size_bytes)
      continue;

    uint64_t bitmap_end = phys + bitmap_size_bytes;
    if (bitmap_end <= phys || bitmap_end > 0x100000000ULL)
      continue;

    bitmap_phys = phys;
    bitmap_found = 1;
    break;
  }

  if (!bitmap_found) {
    LOG_ERR("[PMM] ERROR: No hay RAM por debajo de 4GB para el bitmap!");
    return;
  }

  bitmap = (uint8_t *)bitmap_phys;
  bitmap_phys_global = bitmap_phys;

  memset(bitmap, 0xFF, bitmap_size_bytes);
  used_blocks = max_blocks;

  for (uint64_t i = 0; i < memmap_size; i += memmap_desc_size) {
    uint32_t type = *(uint32_t *)(ptr + i + 0);
    uint64_t phys = *(uint64_t *)(ptr + i + 8);
    uint64_t pages = *(uint64_t *)(ptr + i + 24);

    if (type != EFI_CONVENTIONAL_MEMORY)
      continue;
    uint64_t size;
    if (!pmm_validate_efi_range(phys, pages, &size))
      continue;

    // [PF-SLOW] Registrar el rango usable para el pre-touch de paging.
    if (usable_ranges_count < PMM_MAX_USABLE_RANGES) {
      usable_ranges[usable_ranges_count].start = phys;
      usable_ranges[usable_ranges_count].end = phys + size;
      usable_ranges_count++;
    } else {
      LOG_WARN("[PMM] demasiados rangos usables (>%d); pre-touch se "
               "limitará a los primeros",
               PMM_MAX_USABLE_RANGES);
    }

    uint64_t start_bit = phys / PAGE_SIZE;
    uint64_t end_bit = start_bit + pages;
    if (end_bit < start_bit)
      continue;
    if (end_bit > max_blocks)
      end_bit = max_blocks;

    for (uint64_t bit = start_bit; bit < end_bit; bit++) {
      if (bitmap_test_safe(bitmap, bit)) {
        bitmap_clear_safe(bitmap, bit);
        if (used_blocks > 0)
          used_blocks--;
        total_usable_pages++; // [FIX B] cuenta real de páginas usables
      }
    }
  }

  if (bitmap_in_range(0) && !bitmap_test_safe(bitmap, 0)) {
    bitmap_set_safe(bitmap, 0);
    used_blocks++;
  }

  uint64_t bmp_start_bit = bitmap_phys / PAGE_SIZE;
  uint64_t bitmap_end = bitmap_phys + bitmap_size_bytes;
  if (bitmap_end < bitmap_phys || bitmap_end > UINT64_MAX - (PAGE_SIZE - 1)) {
    LOG_ERR("[PMM] ERROR: rango del bitmap desbordado");
    return;
  }
  uint64_t bmp_end_bit = (bitmap_end + PAGE_SIZE - 1) / PAGE_SIZE;
  for (uint64_t b = bmp_start_bit; b < bmp_end_bit; b++) {
    if (bitmap_in_range(b) && !bitmap_test_safe(bitmap, b)) {
      bitmap_set_safe(bitmap, b);
      used_blocks++;
    }
  }

  /*
   * Never hand legacy low memory to general-purpose clients or DMA. It may
   * contain IVT/BDA, firmware data and startup trampolines even if a faulty
   * EFI map marks some of those pages as conventional. Do this before the
   * buddy allocator is initialized from the shared bitmap.
   */
  uint64_t low_mem_pages = 0x100000ULL / PAGE_SIZE;
  if (low_mem_pages > max_blocks)
    low_mem_pages = max_blocks;
  uint64_t low_mem_new = 0;
  for (uint64_t b = 0; b < low_mem_pages; b++) {
    if (!bitmap_test_safe(bitmap, b)) {
      bitmap_set_safe(bitmap, b);
      used_blocks++;
      low_mem_new++;
    }
  }
  LOG_INFO("[PMM] Reservada memoria baja [0, 1 MiB): %lu páginas nuevas",
           (unsigned long)low_mem_new);

  LOG_INFO(
      "[PMM] Bitmap inicializado en %p. Max RAM: %lu MB, Libres: %lu MB",
      (void *)bitmap_phys, (unsigned long)(max_phys_addr / (1024 * 1024)),
      (unsigned long)((max_blocks - used_blocks) * PAGE_SIZE / (1024 * 1024)));
}

void pmm_relocate_bitmap(void) {
  if (!bitmap_phys_global)
    return;

  bitmap = (uint8_t *)phys_to_virt(bitmap_phys_global);
  LOG_INFO("[PMM] Bitmap relocalizado a la ventana física: %p", (void *)bitmap);

  extern void buddy_init(uint64_t total_pages, uint8_t *bitmap_ptr);
  buddy_init(max_blocks, bitmap);

  // [COW] Array de referencias extra por frame (2 bytes/página).
  uint64_t bytes = max_blocks * sizeof(uint16_t);
  uint64_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t phys = pmm_alloc_pages(npages);
  if (phys) {
    memset(phys_to_virt(phys), 0, npages * PAGE_SIZE);
    page_extra = (uint16_t *)phys_to_virt(phys);
    LOG_INFO("[PMM] Refcount por frame (COW): %lu KB",
             (unsigned long)(npages * PAGE_SIZE / 1024));
  } else {
    LOG_WARN("[PMM] sin memoria para el refcount por frame: fork copiará");
  }
}

int pmm_page_share(uint64_t phys) {
  uint64_t bit = phys / PAGE_SIZE;
  if (!page_extra || !bitmap_in_range(bit))
    return -1;
  uint16_t v = __atomic_load_n(&page_extra[bit], __ATOMIC_RELAXED);
  while (v != 0xFFFF) {
    if (__atomic_compare_exchange_n(&page_extra[bit], &v, v + 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return 0;
  }
  return -1;
}

int pmm_page_is_shared(uint64_t phys) {
  uint64_t bit = phys / PAGE_SIZE;
  if (!page_extra || !bitmap_in_range(bit))
    return 0;
  return __atomic_load_n(&page_extra[bit], __ATOMIC_ACQUIRE) != 0;
}

// Quita una referencia extra si la hay. 1 = quitada, 0 = era la única.
static int pmm_page_unshare(uint64_t phys) {
  uint64_t bit = phys / PAGE_SIZE;
  if (!page_extra || !bitmap_in_range(bit))
    return 0;
  uint16_t v = __atomic_load_n(&page_extra[bit], __ATOMIC_RELAXED);
  while (v != 0) {
    if (__atomic_compare_exchange_n(&page_extra[bit], &v, v - 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return 1;
  }
  return 0;
}

uint64_t pmm_alloc_page(void) {
  if (!bitmap || max_blocks == 0)
    return 0;

  unsigned long flags = spin_lock_irqsave(&pmm_lock);

  uint64_t start_bit = last_alloc_bit;

  for (uint64_t bit = start_bit; bit < max_blocks; bit++) {
    if (!bitmap_test_safe(bitmap, bit)) {
      bitmap_set_safe(bitmap, bit);
      used_blocks++;
      last_alloc_bit = bit + 1;
      spin_unlock_irqrestore(&pmm_lock, flags);
      return bit * PAGE_SIZE;
    }
  }

  for (uint64_t bit = 0; bit < start_bit; bit++) {
    if (!bitmap_test_safe(bitmap, bit)) {
      bitmap_set_safe(bitmap, bit);
      used_blocks++;
      last_alloc_bit = bit + 1;
      spin_unlock_irqrestore(&pmm_lock, flags);
      return bit * PAGE_SIZE;
    }
  }

  spin_unlock_irqrestore(&pmm_lock, flags);

  // [FIX] Rate-limit: loguear como mucho una vez por segundo. Sin esto,
  // un OOM masivo inunda el serial y ralentiza el sistema 100x.
  static volatile uint64_t last_oom_log = 0;
  uint64_t now = sched_get_ticks();
  if (now - last_oom_log >= 1000) {
    last_oom_log = now;
    LOG_ERR("[PMM] Sin páginas libres (used=%lu/%lu)",
            (unsigned long)used_blocks, (unsigned long)max_blocks);
  }
  return 0;
}

uint64_t pmm_alloc_pages(uint64_t count) {
  if (count == 0 || !bitmap || max_blocks == 0)
    return 0;

  unsigned long flags = spin_lock_irqsave(&pmm_lock);

  extern int32_t pages_to_order(uint64_t pages);
  extern uint64_t buddy_alloc_order(uint32_t order);
  int32_t order = pages_to_order(count);
  if (order >= 0) {
    uint64_t addr = buddy_alloc_order((uint32_t)order);
    if (addr) {
      used_blocks += count;
      spin_unlock_irqrestore(&pmm_lock, flags);
      return addr;
    }
  }

  uint64_t start = last_alloc_bit;
  uint64_t run = 0;

  for (uint64_t bit = start; bit < max_blocks; bit++) {
    if (!bitmap_test_safe(bitmap, bit)) {
      run++;
      if (run == count) {
        uint64_t first = bit + 1 - count;
        for (uint64_t b = 0; b < count; b++) {
          bitmap_set_safe(bitmap, first + b);
        }
        used_blocks += count;
        last_alloc_bit = first + count;
        spin_unlock_irqrestore(&pmm_lock, flags);
        return first * PAGE_SIZE;
      }
    } else {
      run = 0;
    }
  }

  run = 0;
  for (uint64_t bit = 0; bit < start; bit++) {
    if (!bitmap_test_safe(bitmap, bit)) {
      run++;
      if (run == count) {
        uint64_t first = bit + 1 - count;
        for (uint64_t b = 0; b < count; b++) {
          bitmap_set_safe(bitmap, first + b);
        }
        used_blocks += count;
        last_alloc_bit = first + count;
        spin_unlock_irqrestore(&pmm_lock, flags);
        return first * PAGE_SIZE;
      }
    } else {
      run = 0;
    }
  }

  spin_unlock_irqrestore(&pmm_lock, flags);
  return 0;
}

uint64_t pmm_alloc_pages_below(uint64_t count, uint64_t limit) {
  if (count == 0 || !bitmap || max_blocks == 0)
    return 0;
  uint64_t align = 1;
  while (align < count)
    align <<= 1;
  uint64_t end = limit / PAGE_SIZE;
  if (end > max_blocks)
    end = max_blocks;

  unsigned long flags = spin_lock_irqsave(&pmm_lock);
  for (uint64_t first = align; first + count <= end; first += align) {
    uint64_t b = 0;
    while (b < count && !bitmap_test_safe(bitmap, first + b))
      b++;
    if (b < count)
      continue;
    for (b = 0; b < count; b++)
      bitmap_set_safe(bitmap, first + b);
    used_blocks += count;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return first * PAGE_SIZE;
  }
  spin_unlock_irqrestore(&pmm_lock, flags);
  return 0;
}

void pmm_free_page(uint64_t phys_addr) {
  if (phys_addr == 0 || !bitmap)
    return;
  // [COW] Frame compartido: solo suelta una referencia.
  if (pmm_page_unshare(phys_addr))
    return;
  unsigned long flags = spin_lock_irqsave(&pmm_lock);
  uint64_t bit = phys_addr / PAGE_SIZE;

  if (bitmap_in_range(bit) && bitmap_test_safe(bitmap, bit)) {
    bitmap_clear_safe(bitmap, bit);
    if (used_blocks > 0)
      used_blocks--;
    if (bit < last_alloc_bit)
      last_alloc_bit = bit;
  } else if (bitmap_in_range(bit)) {
    // [H5] La página ya estaba libre. Suele indicar un bug del llamante
    // (doble free, o free de una dirección nunca asignada). Avisamos,
    // pero no abortamos: en producción el sistema sigue.
    LOG_WARN("[PMM] doble free: phys=%p bit=%lu", (void *)phys_addr,
             (unsigned long)bit);
  }
  spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_free_pages(uint64_t phys_addr, uint64_t count) {
  if (phys_addr == 0 || count == 0 || !bitmap)
    return;
  unsigned long flags = spin_lock_irqsave(&pmm_lock);

  extern int32_t pages_to_order(uint64_t pages);
  extern void buddy_free_order(uint64_t phys_addr, uint32_t order);
  int32_t order = pages_to_order(count);
  if (order >= 0) {
    // [H5] Detectar doble free ANTES de llamar al buddy.
    //
    // buddy_free_order() es idempotente (range_mark_free vuelve a
    // poner a cero bits que ya están a cero), así que sin este
    // chequeo un segundo free del mismo rango decrementa used_blocks
    // por debajo del valor real. Si todas las páginas del rango ya
    // están libres, no hacemos nada y avisamos.
    uint64_t first_bit = phys_addr / PAGE_SIZE;
    int all_free = 1;
    for (uint64_t i = 0; i < count; i++) {
      uint64_t b = first_bit + i;
      if (!bitmap_in_range(b)) {
        all_free = 0;
        break;
      }
      if (bitmap_test_safe(bitmap, b)) { // bit=1 → usada
        all_free = 0;
        break;
      }
    }
    if (all_free) {
      LOG_WARN("[PMM] doble free (rango buddy): phys=%p count=%lu",
               (void *)phys_addr, (unsigned long)count);
      spin_unlock_irqrestore(&pmm_lock, flags);
      return;
    }

    buddy_free_order(phys_addr, (uint32_t)order);
    if (used_blocks >= count)
      used_blocks -= count;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return;
  }

  uint64_t start_bit = phys_addr / PAGE_SIZE;

  for (uint64_t i = 0; i < count; i++) {
    uint64_t b = start_bit + i;
    if (!bitmap_in_range(b))
      continue;
    if (bitmap_test_safe(bitmap, b)) {
      bitmap_clear_safe(bitmap, b);
      if (used_blocks > 0)
        used_blocks--;
    } else {
      LOG_WARN("[PMM] doble free (rango): phys=%p bit=%lu",
               (void *)(b * PAGE_SIZE), (unsigned long)b);
    }
  }
  if (start_bit < last_alloc_bit)
    last_alloc_bit = start_bit;
  spin_unlock_irqrestore(&pmm_lock, flags);
}

// ---------------------------------------------------------------------------
// [SMP] Reserva un rango físico como USADO.
//
// Recorre el bitmap y marca las páginas del rango [start, end) que aún
// estén libres. Ignora las ya reservadas. `end` es exclusivo.
//
// Uso típico: reservar memoria baja para el trampoline SMP antes de
// copiarlo, para que pmm_alloc_page() no la entregue por error.
// ---------------------------------------------------------------------------
void pmm_reserve_range(uint64_t start, uint64_t end) {
  if (!bitmap || max_blocks == 0) {
    LOG_WARN("[PMM] reserve_range: bitmap no inicializado");
    return;
  }
  if (end <= start)
    return;

  unsigned long flags = spin_lock_irqsave(&pmm_lock);

  uint64_t first = start / PAGE_SIZE;
  uint64_t last = (end + PAGE_SIZE - 1) / PAGE_SIZE;

  uint64_t newly_reserved = 0;
  for (uint64_t b = first; b < last; b++) {
    if (bitmap_in_range(b) && !bitmap_test_safe(bitmap, b)) {
      bitmap_set_safe(bitmap, b);
      used_blocks++;
      newly_reserved++;
    }
  }

  spin_unlock_irqrestore(&pmm_lock, flags);

  LOG_INFO("[PMM] Reservado rango [0x%llx, 0x%llx) = %lu páginas nuevas",
           (unsigned long long)start, (unsigned long long)end,
           (unsigned long)newly_reserved);
}

uint64_t pmm_total_pages(void) { return max_blocks; }

uint64_t pmm_free_pages_count(void) {
  return (max_blocks > used_blocks) ? (max_blocks - used_blocks) : 0;
}

int pmm_test_validate_efi_range(uint64_t phys, uint64_t pages,
                                uint64_t *size_out) {
  return pmm_validate_efi_range(phys, pages, size_out);
}

uint64_t pmm_total_usable_pages(void) { return total_usable_pages; }

void pmm_for_each_usable(void (*cb)(uint64_t start, uint64_t end, void *ctx),
                         void *ctx) {
  if (!cb)
    return;
  for (int i = 0; i < usable_ranges_count; i++) {
    cb(usable_ranges[i].start, usable_ranges[i].end, ctx);
  }
}
uint64_t pmm_cow_extra_total(void) {
  uint64_t t = 0;
  if (!page_extra)
    return 0;
  for (uint64_t i = 0; i < max_blocks; i++)
    t += page_extra[i];
  return t;
}
