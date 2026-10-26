/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Manifiesto de un .kur, tal como lo espera el cargador de Aurora. Copia de
 * struct kur_manifest de kernel/kur.h (la capa no incluye cabeceras del
 * kernel): si cambia alli (KUR_ABI_VERSION), hay que cambiarlo aqui.
 */
#ifndef _LKPI_KUR_ABI_H
#define _LKPI_KUR_ABI_H

#include <linux/types.h>

#define LKPI_KUR_MAGIC 0x4B555221u
#define LKPI_KUR_ABI_VERSION 2u
#define LKPI_KUR_MAX_NAME 64

struct lkpi_kur_manifest {
	u32 magic;
	u16 abi_version;
	u16 flags;
	u32 name_len;
	u32 reserved;
	char name[LKPI_KUR_MAX_NAME];
	u8 hash[32];
};

/* Define el manifiesto del modulo; name es un literal de cadena. */
#define LKPI_KUR_MANIFEST(modname) \
	const struct lkpi_kur_manifest __kur_manifest __used = { \
		.magic = LKPI_KUR_MAGIC, \
		.abi_version = LKPI_KUR_ABI_VERSION, \
		.flags = 0, \
		.name_len = sizeof(modname) - 1, \
		.reserved = 0, \
		.name = modname, \
		.hash = { 0 }, \
	}

#endif
