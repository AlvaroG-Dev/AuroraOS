#ifndef _LKPI_CRYPTO_ARC4_H
#define _LKPI_CRYPTO_ARC4_H
#include <linux/types.h>
#include <linux/errno.h>
struct arc4_ctx { u32 S[256]; u32 x, y; };
static inline int arc4_setkey(struct arc4_ctx *ctx, const u8 *in_key, unsigned int key_len) { return -ENOSYS; }
static inline void arc4_crypt(struct arc4_ctx *ctx, u8 *out, const u8 *in, unsigned int len) { }
#endif
