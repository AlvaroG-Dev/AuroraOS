// kernel/ext2.h — driver ext2 (rev 0/1). E1: lectura, E2: escritura. Ver docs/Plan_Ext2.md.
#ifndef KERNEL_EXT2_H
#define KERNEL_EXT2_H

#include "block.h"
#include "vfs.h"

// Monta `bdev_name` (disco o partición) en `mount_path` en solo lectura.
// Rechaza (con el motivo en el log) lo que no
// soporta: journal sin recuperar, extents, 64bit, flex_bg, meta_bg, bloques
// de más de 4 KiB, geometría incoherente. 0 o -errno.
int ext2_mount_bdev(const char *bdev_name, const char *mount_path);

// Igual, pero con rw=1 intenta montar para escritura. Solo se concede si el FS
// se desmontó limpio y usa features que sabemos escribir; si no, -EROFS (el
// motivo queda en el log) y no se monta.
int ext2_mount_bdev_rw(const char *bdev_name, const char *mount_path, int rw);

// remount: rw=1 pasa a escribible (mismas condiciones), rw=0 vuelca y deja el
// FS limpio. 0 o -errno (-EROFS si no se puede escribir).
int ext2_remount(void *fs_priv, int rw);

// Etiqueta del volumen (s_volume_name, hasta 16 caracteres + NUL) de un
// dispositivo con ext2. 0 o -errno.
int ext2_read_label(block_device_t *bdev, char out[17]);

// Variante en dos pasos (la usa mount(2)): ext2_mount valida y devuelve el
// estado; ext2_get_vfs_ops() es el vfs_fs_ops_t para vfs_mount.
int ext2_mount(block_device_t *bdev, void **fs_priv_out);
void ext2_umount(void *fs_priv); // vuelca/invalida la caché y libera
vfs_fs_ops_t *ext2_get_vfs_ops(void);

#endif
