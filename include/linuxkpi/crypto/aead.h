#ifndef _LKPI_CRYPTO_AEAD_H
#define _LKPI_CRYPTO_AEAD_H
#include <linux/crypto.h>
#include <linux/scatterlist.h>
struct crypto_aead { struct lkpi_tfm t; };
struct aead_request { struct crypto_aead *tfm; unsigned int assoclen, cryptlen; struct scatterlist *src, *dst; u8 *iv; char ctx[64]; };
#define AEAD_REQUEST_ON_STACK(name, tfm) struct aead_request name##_buf = { .tfm = tfm }; struct aead_request *name = &name##_buf
static inline struct crypto_aead *crypto_alloc_aead(const char *alg_name, u32 type, u32 mask) { return lkpi_crypto_alloc(0, 16); }
static inline void crypto_free_aead(struct crypto_aead *tfm) { if (!IS_ERR_OR_NULL(tfm)) kfree(tfm); }
static inline int crypto_aead_setkey(struct crypto_aead *tfm, const u8 *key, unsigned int keylen) { tfm->t.keylen = keylen; return 0; }
static inline int crypto_aead_setauthsize(struct crypto_aead *tfm, unsigned int authsize) { tfm->t.authsize = authsize; return 0; }
static inline unsigned int crypto_aead_authsize(struct crypto_aead *tfm) { return tfm->t.authsize; }
static inline unsigned int crypto_aead_reqsize(struct crypto_aead *tfm) { return sizeof(struct aead_request); }
static inline int crypto_aead_encrypt(struct aead_request *req) { return -ENOSYS; }
static inline int crypto_aead_decrypt(struct aead_request *req) { return -ENOSYS; }
static inline void aead_request_set_tfm(struct aead_request *req, struct crypto_aead *tfm) { req->tfm = tfm; }
static inline void aead_request_set_callback(struct aead_request *req, u32 flags, void *compl, void *data) { }
static inline void aead_request_set_crypt(struct aead_request *req, struct scatterlist *src, struct scatterlist *dst, unsigned int cryptlen, u8 *iv) { req->src = src; req->dst = dst; req->cryptlen = cryptlen; req->iv = iv; }
static inline void aead_request_set_ad(struct aead_request *req, unsigned int assoclen) { req->assoclen = assoclen; }
static inline struct aead_request *aead_request_alloc(struct crypto_aead *tfm, gfp_t gfp) { struct aead_request *r = kzalloc(sizeof(*r), gfp); if (r) r->tfm = tfm; return r; }
static inline void aead_request_free(struct aead_request *req) { kfree(req); }
#endif
