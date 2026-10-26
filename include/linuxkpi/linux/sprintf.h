/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vsnprintf de la capa. Formatos C99 habituales mas las extensiones %p de
 * Linux que hacen falta de momento: %pe (nombre del error), %pI4/%pi4,
 * %pM/%pm (MAC), %ph/%phN/%phC/%phD (hex), %pa/%pap (phys_addr_t), %pad
 * (dma_addr_t), %pS/%ps/%pB (direccion, sin simbolos), %pK, %px.
 */
#ifndef _LKPI_LINUX_SPRINTF_H
#define _LKPI_LINUX_SPRINTF_H

#include <linux/compiler.h>
#include <linux/stdarg.h>
#include <linux/types.h>

__printf(3, 0) int lkpi_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
__printf(3, 4) int lkpi_snprintf(char *buf, size_t size, const char *fmt, ...);
__printf(3, 0) int lkpi_vscnprintf(char *buf, size_t size, const char *fmt, va_list args);
__printf(3, 4) int lkpi_scnprintf(char *buf, size_t size, const char *fmt, ...);
__printf(2, 3) int lkpi_sprintf(char *buf, const char *fmt, ...);
__printf(2, 0) int lkpi_vsprintf(char *buf, const char *fmt, va_list args);
__printf(2, 3) char *lkpi_kasprintf(gfp_t gfp, const char *fmt, ...);
__printf(2, 0) char *lkpi_kvasprintf(gfp_t gfp, const char *fmt, va_list args);
__printf(2, 0) const char *lkpi_kvasprintf_const(gfp_t gfp, const char *fmt, va_list args);
int lkpi_sscanf(const char *buf, const char *fmt, ...);
int lkpi_vsscanf(const char *buf, const char *fmt, va_list args);

#define vsnprintf lkpi_vsnprintf
#define snprintf lkpi_snprintf
#define vscnprintf lkpi_vscnprintf
#define scnprintf lkpi_scnprintf
#define sprintf lkpi_sprintf
#define vsprintf lkpi_vsprintf
#define kasprintf lkpi_kasprintf
#define kvasprintf lkpi_kvasprintf
#define kvasprintf_const lkpi_kvasprintf_const
#define sscanf lkpi_sscanf
#define vsscanf lkpi_vsscanf

int lkpi_num_to_str(char *buf, int size, unsigned long long num, unsigned int width);
#define num_to_str lkpi_num_to_str

#endif
