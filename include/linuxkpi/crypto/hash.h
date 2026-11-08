#ifndef _LKPI_CRYPTO_HASH_H
#define _LKPI_CRYPTO_HASH_H
#include <linux/crypto.h>
struct crypto_shash { struct lkpi_tfm t; };
struct shash_desc { struct crypto_shash *tfm; void *__ctx[2]; };
#define SHASH_DESC_ON_STACK(shash, ctx) struct shash_desc shash##_buf; struct shash_desc *shash = &shash##_buf
static inline struct crypto_shash *crypto_alloc_shash(const char *alg_name, u32 type, u32 mask) { return lkpi_crypto_alloc(16, 0); }
static inline void crypto_free_shash(struct crypto_shash *tfm) { if (!IS_ERR_OR_NULL(tfm)) kfree(tfm); }
static inline int crypto_shash_setkey(struct crypto_shash *tfm, const u8 *key, unsigned int keylen) { tfm->t.keylen = keylen; return 0; }
static inline unsigned int crypto_shash_digestsize(struct crypto_shash *tfm) { return tfm->t.digestsize; }
static inline unsigned int crypto_shash_descsize(struct crypto_shash *tfm) { return sizeof(struct shash_desc); }
static inline int crypto_shash_init(struct shash_desc *d) { return -ENOSYS; }
static inline int crypto_shash_update(struct shash_desc *d, const u8 *data, unsigned int len) { return -ENOSYS; }
static inline int crypto_shash_finup(struct shash_desc *d, const u8 *data, unsigned int len, u8 *out) { return -ENOSYS; }
static inline int crypto_shash_digest(struct shash_desc *d, const u8 *data, unsigned int len, u8 *out) { return -ENOSYS; }
#endif
