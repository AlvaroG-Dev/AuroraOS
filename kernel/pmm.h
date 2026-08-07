#pragma once
#include <stdint.h>
#include "paging.h"  // PAGE_SIZE

// Macros auxiliares para bitmap
#define BITMAP_SET(bitmap, bit)   ((bitmap)[(bit) / 8] |= (1 << ((bit) % 8)))
#define BITMAP_CLEAR(bitmap, bit) ((bitmap)[(bit) / 8] &= ~(1 << ((bit) % 8)))
#define BITMAP_TEST(bitmap, bit)  ((bitmap)[(bit) / 8] & (1 << ((bit) % 8)))

void pmm_init(uint64_t memmap, uint64_t memmap_size, uint64_t memmap_desc_size);
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys_addr);
