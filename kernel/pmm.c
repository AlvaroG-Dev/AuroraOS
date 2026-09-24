// kernel/pmm.c
#include "pmm.h"
#include "buddy.h"
#include "klog.h"
#include "paging.h"
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

#define EFI_CONVENTIONAL_MEMORY 7

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

void pmm_init(uint64_t memmap, uint64_t memmap_size,
              uint64_t memmap_desc_size) {
  if (!memmap || memmap_size == 0)
    return;

  spin_init(&pmm_lock);

  uint8_t *ptr = (uint8_t *)memmap;
  uint64_t max_phys_addr = 0;

  for (uint64_t i = 0; i < memmap_size; i += memmap_desc_size) {
    uint32_t type = *(uint32_t *)(ptr + i + 0);
    if (type != EFI_CONVENTIONAL_MEMORY)
      continue;
    uint64_t phys = *(uint64_t *)(ptr + i + 8);
    uint64_t pages = *(uint64_t *)(ptr + i + 24);
    if (pages > UINT64_MAX / PAGE_SIZE)
      continue;
    uint64_t size = pages * PAGE_SIZE;
    if (phys > UINT64_MAX - size)
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
    uint64_t size = pages * PAGE_SIZE;

    if (type == EFI_CONVENTIONAL_MEMORY && phys != 0 &&
        size >= bitmap_size_bytes) {
      if ((phys + bitmap_size_bytes) < 0x100000000ULL) {
        bitmap_phys = phys;
        bitmap_found = 1;
        break;
      }
    }
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

    if (type == EFI_CONVENTIONAL_MEMORY) {
      uint64_t start_bit = phys / PAGE_SIZE;
      for (uint64_t b = 0; b < pages; b++) {
        if (bitmap_in_range(start_bit + b)) {
          bitmap_clear_safe(bitmap, start_bit + b);
          if (used_blocks > 0)
            used_blocks--;
        }
      }
    }
  }

  if (bitmap_in_range(0) && !bitmap_test_safe(bitmap, 0)) {
    bitmap_set_safe(bitmap, 0);
    used_blocks++;
  }

  uint64_t bmp_start_bit = bitmap_phys / PAGE_SIZE;
  uint64_t bmp_end_bit =
      (bitmap_phys + bitmap_size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  for (uint64_t b = bmp_start_bit; b < bmp_end_bit; b++) {
    if (bitmap_in_range(b) && !bitmap_test_safe(bitmap, b)) {
      bitmap_set_safe(bitmap, b);
      used_blocks++;
    }
  }

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
  LOG_ERR("[PMM] ERROR CRITICO: Memoria fisica agotada!");
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

void pmm_free_page(uint64_t phys_addr) {
  if (phys_addr == 0 || !bitmap)
    return;
  unsigned long flags = spin_lock_irqsave(&pmm_lock);
  uint64_t bit = phys_addr / PAGE_SIZE;

  if (bitmap_in_range(bit) && bitmap_test_safe(bitmap, bit)) {
    bitmap_clear_safe(bitmap, bit);
    if (used_blocks > 0)
      used_blocks--;
    if (bit < last_alloc_bit)
      last_alloc_bit = bit;
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
    buddy_free_order(phys_addr, (uint32_t)order);
    if (used_blocks >= count)
      used_blocks -= count;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return;
  }

  uint64_t start_bit = phys_addr / PAGE_SIZE;

  for (uint64_t i = 0; i < count; i++) {
    uint64_t b = start_bit + i;
    if (bitmap_in_range(b) && bitmap_test_safe(bitmap, b)) {
      bitmap_clear_safe(bitmap, b);
      if (used_blocks > 0)
        used_blocks--;
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