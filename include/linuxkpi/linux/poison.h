/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_POISON_H
#define _LKPI_LINUX_POISON_H

/* Valores no canonicos: usarlos tras list_del() provoca un fallo inmediato. */
#define LIST_POISON1 ((void *)0xdead000000000100UL)
#define LIST_POISON2 ((void *)0xdead000000000122UL)
#define POISON_INUSE 0x5a
#define POISON_FREE 0x6b
#define POISON_END 0xa5

#endif
