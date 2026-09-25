// kernel/fat32.h
#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include "block.h"
#include "vfs.h"

// ---------------------------------------------------------------------------
// FAT32 read-only.
//
// Monta un filesystem FAT32 sobre un block_device y expone sus archivos
// y directorios como nodos del VFS. Soporta:
//   - BPB parsing con validación (bytes_per_sector, cluster size, FAT size).
//   - Chain walking vía FAT.
//   - Directorios con 8.3 + Long File Name (LFN) entries.
//   - Lectura de archivos (offset + size).
//
// No soporta (todavía):
//   - Escritura, creación, borrado, rename.
//   - FAT12 / FAT16 (solo FAT32, detectable por fs_type en el BPB).
//   - Mirrors de FAT (usa la primera FAT).
//   - Bad cluster marking (0x0FFFFFF7) más allá de rechazar el acceso.
// ---------------------------------------------------------------------------

// Monta un FS FAT32 sobre `bdev`. Verifica el BPB. Devuelve 0 si OK y
// rellena `*fs_priv_out` con un puntero opaco que el llamante debe pasar
// a vfs_mount y liberar con fat32_umount().
//
// Errores posibles:
//   -EINVAL  BPB inválido / no es FAT32
//   -EIO     no se pudo leer el boot sector
//   -ENOMEM  no hay memoria para la estructura
int fat32_mount(block_device_t *bdev, void **fs_priv_out);

// Libera la estructura de FS. El llamante debe garantizar que no hay
// nodos abiertos apuntando a este FS (cerrar todos los fds antes).
void fat32_umount(void *fs_priv);

// Devuelve el vfs_fs_ops_t para pasar a vfs_mount().
vfs_fs_ops_t *fat32_get_vfs_ops(void);

// Conveniencia: mount + vfs_mount en una sola llamada.
//   - Busca `bdev_name` en el block layer.
//   - Llama a fat32_mount.
//   - Monta en `mount_path` vía vfs_mount.
// Si el vfs_mount falla, libera el fs_priv automáticamente.
// Devuelve 0 si OK.
int fat32_mount_bdev(const char *bdev_name, const char *mount_path);

#endif