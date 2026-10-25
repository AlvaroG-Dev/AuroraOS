// kernel/msi_user.h
#ifndef KERNEL_MSI_USER_H
#define KERNEL_MSI_USER_H

#include "vfs.h"
#include <stdint.h>

// VFS ops de /dev/msi. Se registran en vfs.c (devfs).
extern vfs_ops_t msi_dev_ops;

// ¿Es `vector` del pool de usuario? Si lo es, solo puede abrirlo en
// /dev/irq el proceso `pid` al que se asignó (0 = vector libre).
int aur_msi_is_user_vector(uint32_t vector);
uint32_t aur_msi_vector_pid(uint32_t vector);

#endif
