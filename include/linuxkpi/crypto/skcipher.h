#ifndef _LKPI_CRYPTO_SKCIPHER_H
#define _LKPI_CRYPTO_SKCIPHER_H
#include <linux/crypto.h>
#include <linux/scatterlist.h>
struct crypto_skcipher { struct lkpi_tfm t; };
struct skcipher_request { struct crypto_skcipher *tfm; char ctx[64]; };
static inline struct crypto_skcipher *crypto_alloc_skcipher(const char *alg_name, u32 type, u32 mask) { return lkpi_crypto_alloc(0, 0); }
static inline void crypto_free_skcipher(struct crypto_skcipher *tfm) { if (!IS_ERR_OR_NULL(tfm)) kfree(tfm); }
static inline int crypto_skcipher_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen) { return 0; }
static inline int crypto_skcipher_encrypt(struct skcipher_request *r) { return -ENOSYS; }
static inline int crypto_skcipher_decrypt(struct skcipher_request *r) { return -ENOSYS; }
static inline struct skcipher_request *skcipher_request_alloc(struct crypto_skcipher *tfm, gfp_t gfp) { struct skcipher_request *r = kzalloc(sizeof(*r), gfp); if (r) r->tfm = tfm; return r; }
static inline void skcipher_request_free(struct skcipher_request *r) { kfree(r); }
static inline void skcipher_request_set_crypt(struct skcipher_request *r, struct scatterlist *src, struct scatterlist *dst, unsigned int cryptlen, void *iv) { }
static inline void skcipher_request_set_callback(struct skcipher_request *r, u32 flags, void *compl, void *data) { }
static inline void skcipher_request_set_tfm(struct skcipher_request *r, struct crypto_skcipher *tfm) { r->tfm = tfm; }
#endif
