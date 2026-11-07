/* Verificacion de firmas (regdb firmada): no soportada. */
#ifndef _LKPI_LINUX_VERIFICATION_H
#define _LKPI_LINUX_VERIFICATION_H
#include <linux/errno.h>
#include <linux/types.h>
struct key;
#define VERIFY_USE_SECONDARY_KEYRING ((struct key *)1UL)
#define VERIFY_USE_PLATFORM_KEYRING ((struct key *)2UL)
static inline int verify_pkcs7_signature(const void *data, size_t len, const void *raw_pkcs7, size_t pkcs7_len, struct key *trusted_keys, int usage, void *view_content, void *ctx) { return -EOPNOTSUPP; }
#define VERIFYING_UNSPECIFIED_SIGNATURE 0
#endif
