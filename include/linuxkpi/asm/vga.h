/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ASM_VGA_H
#define _LKPI_ASM_VGA_H

#include <asm/io.h>

/* La memoria VGA legada no se toca: la capa solo da direcciones para compilar */
#define VGA_MAP_MEM(x, s) ((unsigned long)phys_to_virt(x))
#define vga_readb(x) (*(x))
#define vga_writeb(x, y) (*(y) = (x))

#endif
