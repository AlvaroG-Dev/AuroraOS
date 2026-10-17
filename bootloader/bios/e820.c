/* bootloader/bios/e820.c
 *
 * Convertir E820 (formato BIOS) a EFI_MEMORY_DESCRIPTOR[].
 * El kernel reconoce type==7 como EfiConventionalMemory. E820 crudo
 * NO funciona (type 1 es "usable" en E820, no en EFI).
 *
 * Ademas: tenemos que restar de los rangos "usable" las zonas que el
 * propio bootloader ocupa (page tables, memmap, kernel cargado, initrd),
 * para que el PMM no las reclame.
 */
#include "bios.h"

#define MAX_RANGES 256

typedef struct {
  uint64_t start;
  uint64_t end;
  uint32_t type;
} range_t;

static uint32_t e820_type_to_efi(uint32_t t) {
  switch (t) {
  case 1:
    return EFI_CONVENTIONAL_MEMORY;
  case 2:
    return EFI_RESERVED_MEMORY_TYPE;
  case 3:
    return EFI_ACPI_RECLAIM_MEMORY;
  case 4:
    return EFI_ACPI_MEMORY_NVS;
  case 5:
    return EFI_UNUSABLE_MEMORY;
  default:
    return EFI_RESERVED_MEMORY_TYPE;
  }
}

/* Inserta [start,end,type] en el array ordenado, fusionando cuando
 * sea posible. */
static void ranges_insert(range_t *r, uint32_t *n, uint64_t start, uint64_t end,
                          uint32_t type) {
  if (end <= start)
    return;
  for (uint32_t i = 0; i < *n; i++) {
    if (r[i].end == start && r[i].type == type) {
      r[i].end = end;
      return;
    }
    if (r[i].start == end && r[i].type == type) {
      r[i].start = start;
      return;
    }
  }
  if (*n < MAX_RANGES) {
    r[*n].start = start;
    r[*n].end = end;
    r[*n].type = type;
    (*n)++;
  }
}

/* Elimina [cut_start, cut_end) de [*start, *end), recortando o partiendo. */
static void ranges_cut(range_t *r, uint32_t *n, uint64_t cut_start,
                       uint64_t cut_end) {
  if (cut_end <= cut_start)
    return;
  for (uint32_t i = 0; i < *n;) {
    range_t *e = &r[i];
    if (e->end <= cut_start || e->start >= cut_end) {
      i++;
      continue;
    }

    if (e->start < cut_start && e->end > cut_end) {
      /* Partir en dos. */
      range_t tail = {cut_end, e->end, e->type};
      e->end = cut_start;
      if (*n < MAX_RANGES) {
        r[*n] = tail;
        (*n)++;
      }
      i++;
      continue;
    }
    if (e->start < cut_start)
      e->end = cut_start;
    else if (e->end > cut_end)
      e->start = cut_end;
    else {
      /* Totalmente contenido: eliminar. */
      for (uint32_t j = i; j + 1 < *n; j++)
        r[j] = r[j + 1];
      (*n)--;
      continue;
    }
    i++;
  }
}

uint32_t e820_to_efi(const e820_entry_t *e820, uint32_t count, uint8_t *out,
                     uint32_t out_size, const uint64_t *reserved_ranges,
                     uint32_t num_reserved) {
  range_t ranges[MAX_RANGES];
  uint32_t n = 0;

  for (uint32_t i = 0; i < count; i++) {
    uint64_t s = e820[i].base;
    uint64_t e = s + e820[i].length;
    if (e < s)
      continue;
    ranges_insert(ranges, &n, s, e, e820_type_to_efi(e820[i].type));
  }

  /* Restar rangos reservados por el bootloader. */
  for (uint32_t i = 0; i < num_reserved; i += 2) {
    ranges_cut(ranges, &n, reserved_ranges[i], reserved_ranges[i + 1]);
  }

  /* Emitir. */
  uint32_t emitted = 0;
  uint32_t off = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint64_t s = ranges[i].start;
    uint64_t e = ranges[i].end;
    uint64_t pages = (e - s) / 0x1000;
    if (pages == 0)
      continue;

    if (off + sizeof(efi_memory_descriptor_t) > out_size)
      break;

    efi_memory_descriptor_t d;
    d.type = ranges[i].type;
    d.pad = 0;
    d.phys_start = s;
    d.virt_start = s; /* identity mientras no haya paging */
    d.num_pages = pages;
    d.attr = 0;

    copy_mem(out + off, &d, sizeof(d));
    off += sizeof(d);
    emitted++;
  }
  return off;
}