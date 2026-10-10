// kernel/net/checksum.c

#include "checksum.h"
#include "byteorder.h"

uint32_t inet_csum_partial(const void *data, size_t len, uint32_t initial) {
  uint32_t sum = initial;
  const uint8_t *p = (const uint8_t *)data;

  // Suma de palabras de 16 bits en network order (big-endian).
  // x86 lee uint16_t en little-endian, así que leemos byte a byte
  // (o ntohs).
  while (len >= 2) {
    uint16_t w = (uint16_t)((p[0] << 8) | p[1]);
    sum += w;
    p += 2;
    len -= 2;
  }

  // Byte impar: se rellena con 0 a la derecha (es el byte alto de la
  // última palabra de 16 bits en network order).
  if (len == 1) {
    sum += (uint32_t)(p[0] << 8);
  }

  return sum;
}

uint16_t inet_csum(const void *data, size_t len) {
  uint32_t sum = inet_csum_partial(data, len, 0);
  while (sum >> 16)
    sum = (sum & 0xFFFFu) + (sum >> 16);
  return (uint16_t)(~sum & 0xFFFFu);
}

uint16_t inet_csum_pseudo(uint32_t saddr, uint32_t daddr, uint8_t proto,
                          const void *data, size_t len) {
  // Pseudo-header (RFC 768 / RFC 793):
  //   +--------+--------+--------+--------+
  //   |         Source IP (32)            |
  //   +--------+--------+--------+--------+
  //   |       Destination IP (32)         |
  //   +--------+--------+--------+--------+
  //   |  zero  |  proto |   length (16)   |
  //   +--------+--------+--------+--------+
  uint8_t pseudo[12];
  pseudo[0] = (uint8_t)((saddr >> 24) & 0xFF);
  pseudo[1] = (uint8_t)((saddr >> 16) & 0xFF);
  pseudo[2] = (uint8_t)((saddr >> 8) & 0xFF);
  pseudo[3] = (uint8_t)(saddr & 0xFF);
  pseudo[4] = (uint8_t)((daddr >> 24) & 0xFF);
  pseudo[5] = (uint8_t)((daddr >> 16) & 0xFF);
  pseudo[6] = (uint8_t)((daddr >> 8) & 0xFF);
  pseudo[7] = (uint8_t)(daddr & 0xFF);
  pseudo[8] = 0;
  pseudo[9] = proto;
  pseudo[10] = (uint8_t)((len >> 8) & 0xFF);
  pseudo[11] = (uint8_t)(len & 0xFF);

  uint32_t sum = inet_csum_partial(pseudo, sizeof(pseudo), 0);
  sum = inet_csum_partial(data, len, sum);
  while (sum >> 16)
    sum = (sum & 0xFFFFu) + (sum >> 16);
  return (uint16_t)(~sum & 0xFFFFu);
}

int inet_csum_verify(const void *data, size_t len) {
  // Para un buffer con el checksum ya incluido, el checksum completo
  // (incluyendo el campo) debe dar 0.
  uint32_t sum = inet_csum_partial(data, len, 0);
  while (sum >> 16)
    sum = (sum & 0xFFFFu) + (sum >> 16);
  return ((uint16_t)(~sum & 0xFFFFu) == 0) ? 0 : -1;
}

int inet_csum_pseudo_verify(uint32_t saddr, uint32_t daddr, uint8_t proto,
                            const void *data, size_t len) {
  uint16_t c = inet_csum_pseudo(saddr, daddr, proto, data, len);
  return (c == 0) ? 0 : -1;
}