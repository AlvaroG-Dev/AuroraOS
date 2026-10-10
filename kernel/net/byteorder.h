// kernel/net/byteorder.h
//
// Conversiones de byte order para el stack de red. Todo el protocolo
// IP es big-endian (network order); x86 es little-endian. Estos
// helpers hacen la traducción en el sitio (inline).
//
// Convención: los tipos uint16_t/uint32_t en el stack de red están
// SIEMPRE en host order. La conversión se hace al leer/escribir de
// memoria (structs en wire format).

#ifndef KERNEL_NET_BYTEORDER_H
#define KERNEL_NET_BYTEORDER_H

#include <stdint.h>

static inline uint16_t bswap16(uint16_t x) {
  return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint32_t bswap32(uint32_t x) {
  return ((x >> 24) & 0x000000FFu) | ((x >> 8) & 0x0000FF00u) |
         ((x << 8) & 0x00FF0000u) | ((x << 24) & 0xFF000000u);
}

static inline uint64_t bswap64(uint64_t x) {
  return ((uint64_t)bswap32((uint32_t)(x & 0xFFFFFFFFu)) << 32) |
         (uint64_t)bswap32((uint32_t)(x >> 32));
}

// host → network (big-endian)
static inline uint16_t htons(uint16_t x) { return bswap16(x); }
static inline uint32_t htonl(uint32_t x) { return bswap32(x); }
static inline uint64_t htonll(uint64_t x) { return bswap64(x); }

// network → host
static inline uint16_t ntohs(uint16_t x) { return bswap16(x); }
static inline uint32_t ntohl(uint32_t x) { return bswap32(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap64(x); }

#endif