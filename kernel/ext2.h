// kernel/ext2.h — driver ext2 (rev 0/1). E1: solo lectura. Ver docs/Plan_Ext2.md.
#ifndef KERNEL_EXT2_H
#define KERNEL_EXT2_H

#include "block.h"
#include "vfs.h"

// Monta `bdev_name` (disco o partición) en `mount_path`, SIEMPRE en solo
// lectura mientras no exista E2. Rechaza (con el motivo en el log) lo que no
// soporta: journal sin recuperar, extents, 64bit, flex_bg, meta_bg, bloques
// de más de 4 KiB, geometría incoherente. 0 o -errno.
int ext2_mount_bdev(const char *bdev_name, const char *mount_path);

// Variante en dos pasos (la usa mount(2)): ext2_mount valida y devuelve el
// estado; ext2_get_vfs_ops() es el vfs_fs_ops_t para vfs_mount.
int ext2_mount(block_device_t *bdev, void **fs_priv_out);
void ext2_umount(void *fs_priv); // vuelca/invalida la caché y libera
vfs_fs_ops_t *ext2_get_vfs_ops(void);

#endif
