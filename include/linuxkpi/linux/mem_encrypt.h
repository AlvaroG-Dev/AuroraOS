/* SPDX-License-Identifier: GPL-2.0-only */
/* Cifrado de memoria (SME/SEV): inactivo. */
#ifndef _LKPI_LINUX_MEM_ENCRYPT_H
#define _LKPI_LINUX_MEM_ENCRYPT_H

#include <linux/cc_platform.h>

#define mem_encrypt_active() false
#define pgprot_decrypted(prot) (prot)
#define pgprot_encrypted(prot) (prot)

#endif
