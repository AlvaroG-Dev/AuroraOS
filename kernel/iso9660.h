// kernel/iso9660.h
#ifndef KERNEL_ISO9660_H
#define KERNEL_ISO9660_H

#include "vfs.h"

// Monta un device block con un filesystem ISO9660 en `mountpoint`.
// Soporta Rock Ridge (nombres largos + flags POSIX) si el volumen lo
// trae. Sin Rock Ridge, usa los nombres 8.3 de la base ISO9660 con el
// sufijo ";1" recortado.
//
// El device debe tener sector_size = 2048 (CDs, DVDs, BDs).
//
// Devuelve 0 si OK, negativo si error (mount ya existe, no es ISO9660,
// no se pudo leer el device, etc.).
int iso9660_mount_bdev(const char *bdev_name, const char *mountpoint);

// Expuesta para que un futuro `mount(1)` pueda registrar/desregistrar
// el FS. Uso interno desde vfs.c.
struct vfs_fs_ops *iso9660_get_vfs_ops(void);

#endif