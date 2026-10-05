// kernel/swap.h
#ifndef SWAP_H
#define SWAP_H

#include <stdbool.h>
#include <stdint.h>


struct block_device;

// ---------------------------------------------------------------------------
// Swap: extensión de RAM a disco.
//
// Cada swap_device tiene N slots de PAGE_SIZE bytes. El slot k se
// almacena en el byte offset (k + 1) * PAGE_SIZE del device (Linux
// format: la primera página es el header).
//
// Referencias usan swap entries codificadas en el PTE:
//   bit 0     = 0  (not present)
//   bit 6     = 1  (swap marker, posicion de PTE_DIRTY para no-presentes)
//   bits 7-14 = type (0..255)
//   bits 15+  = slot offset dentro del device
// ---------------------------------------------------------------------------

#define SWAP_MAX_DEVICES 32
#define SWAP_MAGIC "SWAPSPACE2"
#define SWAP_MAGIC_LEN 10
#define SWAP_HEADER_OFFSET 1024
#define SWAP_MAGIC_OFFSET 4086 // PAGE_SIZE - 10
#define SWAP_VERSION_OFFSET (SWAP_HEADER_OFFSET + 0)
#define SWAP_LASTPAGE_OFFSET (SWAP_HEADER_OFFSET + 4)
#define SWAP_NR_BADPAGES_OFFSET (SWAP_HEADER_OFFSET + 8)

typedef struct swap_device {
  struct block_device *bdev;
  uint32_t type;        // 0..SWAP_MAX_DEVICES-1
  uint32_t nr_slots;    // slots totales (indices 0..nr_slots-1)
  uint32_t next_slot;   // hint para el siguiente alloc
  uint32_t used_slots;  // slots en uso
  uint8_t *slot_bitmap; // ceil(nr_slots/8) bytes
  uint64_t total_bytes;
  int in_use;
  int frozen; // si 1, no aceptar nuevas allocs (durante swapoff)
  struct swap_device *next;
} swap_device_t;

// ---------------------------------------------------------------------------
// Encoding/decoding de swap entries en PTEs.
// ---------------------------------------------------------------------------
#define PTE_SWAP_MARKER (1ULL << 6)
#define PTE_SWAP_TYPE_SHIFT 7
#define PTE_SWAP_TYPE_MASK 0xFFULL
#define PTE_SWAP_OFFSET_SHIFT 15

static inline uint64_t pte_encode_swap(uint32_t type, uint32_t offset) {
  return PTE_SWAP_MARKER |
         ((uint64_t)(type & PTE_SWAP_TYPE_MASK) << PTE_SWAP_TYPE_SHIFT) |
         ((uint64_t)offset << PTE_SWAP_OFFSET_SHIFT);
}

static inline int pte_is_swap(uint64_t pte) {
  return (pte & 1) == 0 && (pte & PTE_SWAP_MARKER) != 0;
}

static inline uint32_t pte_swap_type(uint64_t pte) {
  return (uint32_t)((pte >> PTE_SWAP_TYPE_SHIFT) & PTE_SWAP_TYPE_MASK);
}

static inline uint32_t pte_swap_offset(uint64_t pte) {
  return (uint32_t)(pte >> PTE_SWAP_OFFSET_SHIFT);
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void swap_init(void);

int swap_on(const char *path); // "sdb3" o "/dev/sdb3"
int swap_off(const char *path);

uint32_t swap_alloc_slot(uint32_t type);
void swap_free_slot(uint32_t type, uint32_t slot);
int swap_write_page(uint32_t type, uint32_t slot, uint64_t phys);
int swap_read_page(uint32_t type, uint32_t slot, uint64_t phys);

uint64_t swap_total_bytes(void);
uint64_t swap_used_bytes(void);
uint64_t swap_free_bytes(void);
int swap_device_count(void);

// Reclaim: intenta swapear UNA página anónima. Devuelve 1 si lo logró.
int swap_reclaim_one(void);

// Entry point de kswapd.
void kswapd_main(void);

// Iterador para /proc/swaps.
typedef void (*swap_iter_cb_t)(const char *devname, uint64_t total_kb,
                               uint64_t used_kb, uint64_t free_kb, void *arg);
void swap_for_each(swap_iter_cb_t cb, void *arg);

#endif