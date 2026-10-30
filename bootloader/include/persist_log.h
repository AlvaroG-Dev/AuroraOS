/* bootloader/include/persist_log.h
 *
 * Log del kernel en RAM que sobrevive a un reinicio caliente (estilo
 * ramoops). Lo comparten el bootloader UEFI y el kernel.
 *
 * El bootloader reserva PLOG_PAGES paginas (EfiReservedMemoryType, fuera del
 * PMM) en un sitio estable y las divide en dos ranuras de PLOG_SLOT_SIZE:
 *   ranura 0 = log del arranque actual (la escribe el kernel)
 *   ranura 1 = log del arranque anterior (copiada por el bootloader)
 * Cada ranura = cabecera + buffer circular de bytes.
 */
#ifndef AURORA_PERSIST_LOG_H
#define AURORA_PERSIST_LOG_H

#include <stdint.h>

#define PLOG_MAGIC 0x31474F4C41525541ULL /* "AURALOG1" */
#define PLOG_VERSION 1
#define PLOG_SLOT_SIZE (128 * 1024)
#define PLOG_PAGES (2 * PLOG_SLOT_SIZE / 4096)
#define PLOG_HDR_SIZE 64
#define PLOG_CAPACITY (PLOG_SLOT_SIZE - PLOG_HDR_SIZE)

struct plog_header {
  uint64_t magic;
  uint32_t version;
  uint32_t capacity; /* bytes del buffer circular */
  uint32_t head;     /* proxima posicion de escritura (< capacity) */
  uint32_t check;    /* plog_check() de los campos anteriores */
  uint64_t total;    /* bytes escritos desde el arranque (si > capacity, dio la
                        vuelta) */
  uint64_t boot_seq; /* numero de arranque con region viva */
  uint8_t pad[24];
};

_Static_assert(sizeof(struct plog_header) == PLOG_HDR_SIZE,
               "plog_header debe medir 64 bytes");

static inline uint32_t plog_check(const struct plog_header *h) {
  return (uint32_t)(h->magic ^ (h->magic >> 32)) ^ h->version ^ h->capacity ^
         0xA5A5A5A5u;
}

static inline uint8_t *plog_data(struct plog_header *h) {
  return (uint8_t *)h + PLOG_HDR_SIZE;
}

static inline void plog_init(struct plog_header *h, uint64_t boot_seq) {
  h->magic = PLOG_MAGIC;
  h->version = PLOG_VERSION;
  h->capacity = PLOG_CAPACITY;
  h->head = 0;
  h->check = plog_check(h);
  h->total = 0;
  h->boot_seq = boot_seq;
}

/* 1 si la cabecera tiene pinta de haber sido escrita por nosotros. */
static inline int plog_valid(const struct plog_header *h) {
  return h->magic == PLOG_MAGIC && h->version == PLOG_VERSION &&
         h->capacity == PLOG_CAPACITY && h->head < h->capacity &&
         h->check == plog_check(h);
}

/* Bytes utiles del log (<= capacity). */
static inline uint32_t plog_len(const struct plog_header *h) {
  return h->total > h->capacity ? h->capacity : (uint32_t)h->total;
}

/* Copia el log en orden cronologico a out (hasta max bytes). */
static inline uint32_t plog_linear(struct plog_header *h, uint8_t *out,
                                   uint32_t max) {
  uint32_t len = plog_len(h);
  uint32_t start = h->total > h->capacity ? h->head : 0;
  uint8_t *d = plog_data(h);
  uint32_t n = len < max ? len : max;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t p = start + i;
    if (p >= h->capacity)
      p -= h->capacity;
    out[i] = d[p];
  }
  return n;
}

#endif
