/* API criptografica de Linux: STUB para mac80211 (Plan_Red.md N7). Los objetos se
 * crean y aceptan claves, pero cifrar/descifrar por software devuelve -ENOSYS: el
 * cifrado de Wi-Fi (CCMP/GCMP/BIP) lo hace el hardware del adaptador (iwlmvm). */
#ifndef _LKPI_LINUX_CRYPTO_H
#define _LKPI_LINUX_CRYPTO_H
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>
#define CRYPTO_MAX_ALG_NAME 128
#define CRYPTO_ALG_ASYNC 0x80
#define CRYPTO_TFM_REQ_MAY_SLEEP 0x200
struct lkpi_tfm { unsigned int authsize; unsigned int reqsize; unsigned int keylen; unsigned int digestsize; };
static inline void *lkpi_crypto_alloc(unsigned int digestsize, unsigned int authsize)
{ struct lkpi_tfm *t = kzalloc(sizeof(*t), GFP_KERNEL); if (!t) return ERR_PTR(-ENOMEM); t->digestsize = digestsize; t->authsize = authsize; t->reqsize = 64; return t; }
#endif
