/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_INIT_H
#define _LKPI_LINUX_INIT_H

#include <linux/compiler.h>
#include <linux/types.h>

/* En un .kur no se libera la memoria de init: las marcas no tienen efecto. */
#define __init
#define __exit
#define __initdata
#define __initconst
#define __exitdata
#define __refdata
#define __ref
#define __meminit
#define __meminitdata
#define __devinit
#define __devexit
#define __exit_p(x) x
#define __init_memblock

#endif
