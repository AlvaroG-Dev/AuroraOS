/* bootloader/bios/bump.c
 *
 * Allocator de páginas físicas para el bootloader BIOS. Devuelve
 * páginas libres según E820, evitando:
 *   - La zona baja (0..KERNEL_STAGE_END) reservada para BIOS, stages,
 *     page tables iniciales y el staging del kernel.
 *   - Cruzar huecos no-usables de E820.
 */
#include "bios.h"

#define RESERVED_LOW_BASE 0x00000000u
#define KERNEL_STAGE_END (KERNEL_STAGE_ADDR + KERNEL_STAGE_MAX)

typedef struct {
  uint64_t start;
  uint64_t end;
} usable_t;

static usable_t g_usable[64];
static uint32_t g_usable_n = 0;
static uint32_t g_cur_range = 0;
static uint64_t g_next = 0;

void bump_init(const e820_entry_t *e820, uint32_t count) {
  g_usable_n = 0;
  g_cur_range = 0;
  g_next = 0;

  for (uint32_t i = 0; i < count; i++) {
    if (e820[i].type != 1)
      continue; /* usable */
    uint64_t s = e820[i].base;
    uint64_t e = s + e820[i].length;

    /* Saltar todo lo que esté por debajo de KERNEL_STAGE_END. */
    if (e <= KERNEL_STAGE_END)
      continue;
    if (s < KERNEL_STAGE_END)
      s = KERNEL_STAGE_END;

    if (g_usable_n < 64) {
      g_usable[g_usable_n].start = s;
      g_usable[g_usable_n].end = e;
      g_usable_n++;
    }
  }
  if (g_usable_n > 0)
    g_next = g_usable[0].start;
}

uint32_t bump_alloc_pages(uint32_t n) {
  if (n == 0)
    return 0;
  uint64_t need = (uint64_t)n * 0x1000;

  while (g_cur_range < g_usable_n) {
    usable_t *r = &g_usable[g_cur_range];
    if (g_next < r->start)
      g_next = r->start;
    if (g_next + need <= r->end) {
      uint32_t result = (uint32_t)g_next;
      g_next += need;
      return result;
    }
    g_cur_range++;
    if (g_cur_range < g_usable_n)
      g_next = g_usable[g_cur_range].start;
  }
  return 0;
}

uint32_t bump_alloc_page(void) { return bump_alloc_pages(1); }

/* Posición actual del bump. Para reservar en el memmap final. */
uint32_t bump_current(void) { return (uint32_t)g_next; }

static uint32_t g_low_bump = 0x80000;
uint32_t bump_alloc_low(uint32_t bytes, uint32_t align) {
  uint32_t a = (g_low_bump + align - 1) & ~(align - 1);
  if (a + bytes > 0x9F000)
    return 0;
  g_low_bump = a + bytes;
  return a;
}