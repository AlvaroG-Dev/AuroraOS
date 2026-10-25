// kernel/block_proxy.h
#ifndef KERNEL_BLOCK_PROXY_H
#define KERNEL_BLOCK_PROXY_H

#include "vfs.h"
#include <stdint.h>

// VFS ops de /dev/blkproxy. Se registran en vfs.c (devfs).
extern vfs_ops_t blkp_dev_ops;

// Inicializa el estado global. Idempotente; se llama desde vfs_init().
void blkp_init(void);

// Bloquea hasta que el driver de bloque de usuario haya registrado sus
// discos (BLKP_READY) o hasta timeout_ms. Devuelve 1 si estaba listo,
// 0 si expiró. Solo tiene sentido si hay un driver de bloque .aur esperado
// (ver ahci_user_mode()).
int blkp_wait_ready(uint32_t timeout_ms);

// El arranque ya ha escaneado particiones. A partir de aquí, los discos
// que lleguen tarde (BLKP_READY posterior) se escanean en un kthread.
void blkp_boot_scanned(void);

#endif
