// kernel/fat32.h
#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include "block.h"
#include "vfs.h"

// ---------------------------------------------------------------------------
// [2.4] Resultado de un scan activo. Reporta inconsistencias sin
// arreglarlas. Lo consume aurora-fsck vía syscall ASYS_FS_CHECK.
// ---------------------------------------------------------------------------
struct fat32_check_result {
  uint64_t clusters_total;     // fat_entries
  uint64_t clusters_free;      // libres según FAT
  uint64_t clusters_used;      // no-libres según FAT
  uint64_t clusters_reachable; // alcanzables desde root
  uint64_t clusters_orphan;    // = used - reachable
  uint64_t dirs_visited;
  uint64_t files_visited;
  uint64_t broken_chains; // dirent apunta a cluster libre/inválido
  uint64_t loops_detected;
  uint64_t fsinfo_free; // valor reportado por FSInfo (si existe)
  uint64_t fsinfo_next; // valor reportado por FSInfo
  int fsinfo_matches;   // 1 si free_count coincide con clusters_free
  int _pad[3];
};

// Scan activo. Recorre el árbol desde root, marca clusters alcanzables,
// cuenta huérfanos/loops/cadenas rotas. Devuelve 0 si el scan corrió
// (aunque encuentre problemas), negativo en error de E/S.
int fat32_check(void *fs_priv, struct fat32_check_result *out);

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