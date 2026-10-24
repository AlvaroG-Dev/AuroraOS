// kernel/sha256.h
#ifndef KERNEL_SHA256_H
#define KERNEL_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t  buffer[64];
  size_t   buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[32]);
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#endif  // KERNEL_SHA256_H