/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Swap sin disco (L6): la capa no tiene dispositivo de swap. "Espacio de
 * swap" es la RAM que aun queda libre para los ficheros shmem de respaldo
 * (ttm_backup): la mitad de las paginas libres del PMM, y nunca mas. Las
 * paginas "expulsadas" siguen en RAM, pero fuera de las listas de la VRAM/GTT
 * del driver y de los pools de TTM, y se pueden recuperar con shmem.
 */
#ifndef _LKPI_LINUX_SWAP_H
#define _LKPI_LINUX_SWAP_H

#include <linux/mm.h>
#include <linux/shmem_fs.h>

long lkpi_nr_swap_pages(void);
#define get_nr_swap_pages() lkpi_nr_swap_pages()
#define total_swap_pages lkpi_nr_swap_pages()

#endif
