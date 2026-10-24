// kernel/sha256.c
//
// SHA-256 FIPS 180-4, portátil, sin dependencias del kernel. Usado por
// kur_loader (verificación de hash del .kur) y por tools/kurbuild
// (cálculo en el host). Sin tabla dinámica, sin allocs.
#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

static void transform(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) |
           ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) |
           ((uint32_t)block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx) {
  ctx->state[0] = 0x6a09e667u;
  ctx->state[1] = 0xbb67ae85u;
  ctx->state[2] = 0x3c6ef372u;
  ctx->state[3] = 0xa54ff53au;
  ctx->state[4] = 0x510e527fu;
  ctx->state[5] = 0x9b05688cu;
  ctx->state[6] = 0x1f83d9abu;
  ctx->state[7] = 0x5be0cd19u;
  ctx->bitlen = 0;
  ctx->buflen = 0;
}

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i < len) {
    if (ctx->buflen == 0 && len - i >= 64) {
      transform(ctx->state, data + i);
      ctx->bitlen += 512;
      i += 64;
      continue;
    }
    ctx->buffer[ctx->buflen++] = data[i++];
    if (ctx->buflen == 64) {
      transform(ctx->state, ctx->buffer);
      ctx->bitlen += 512;
      ctx->buflen = 0;
    }
  }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[32]) {
  uint32_t i = (uint32_t)ctx->buflen;
  if (i < 56) {
    ctx->buffer[i++] = 0x80;
    while (i < 56) ctx->buffer[i++] = 0;
  } else {
    ctx->buffer[i++] = 0x80;
    while (i < 64) ctx->buffer[i++] = 0;
    transform(ctx->state, ctx->buffer);
    memset(ctx->buffer, 0, 56);
  }
  uint64_t bits = ctx->bitlen + (uint64_t)ctx->buflen * 8;
  for (int j = 0; j < 8; j++)
    ctx->buffer[56 + j] = (uint8_t)(bits >> (56 - 8 * j));
  transform(ctx->state, ctx->buffer);
  for (int j = 0; j < 8; j++) {
    out[j * 4 + 0] = (uint8_t)(ctx->state[j] >> 24);
    out[j * 4 + 1] = (uint8_t)(ctx->state[j] >> 16);
    out[j * 4 + 2] = (uint8_t)(ctx->state[j] >> 8);
    out[j * 4 + 3] = (uint8_t)(ctx->state[j]);
  }
}

void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  sha256_ctx_t ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, out);
}