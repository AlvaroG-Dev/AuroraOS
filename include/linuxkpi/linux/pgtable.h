/* SPDX-License-Identifier: GPL-2.0-only */
/* Las operaciones sobre pgprot_t viven en mm.h (K8); aqui solo el nombre de la cabecera. */
#ifndef _LKPI_LINUX_PGTABLE_H
#define _LKPI_LINUX_PGTABLE_H

#include <linux/mm.h>
#include <asm/processor.h>


#define _PAGE_BIT_PAT	7
#define _PAGE_PAT	(1UL << _PAGE_BIT_PAT)
#endif
