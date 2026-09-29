// kernel/tarfs.h
#ifndef TARFS_H
#define TARFS_H

#include <stddef.h>
#include <stdint.h>

// [3.1] tarfs_fs_lookup devuelve un vfs_node_t; el tipo completo se
// forward-declara aquí para no arrastrar vfs.h a todo el que incluya
// tarfs.h. Solo vfs.c necesita el tipo completo.
struct vfs_fs_ops;

typedef struct {
  char name[256];
  uint8_t *data;
  size_t size;
  int is_dir;
  int is_symlink;
  char linkname[100];

  // [3.4.a] Metadatos del header ustar.
  uint32_t mode; // bits rwx (sin S_IF*)
  uint32_t uid;
  uint32_t gid;
} tar_node_t;

void tarfs_init(const void *tar_addr, size_t tar_size);
tar_node_t *tarfs_open(const char *path);
void tarfs_list(const char *dir_path);
size_t tarfs_get_node_count(void);
tar_node_t *tarfs_get_node(size_t index);
tar_node_t *tar_find_file(const char *path);

// [3.1] Devuelve las vfs_fs_ops de tarfs para pasarlas a vfs_mount().
// Permite que vfs.c monte tarfs sin saber nada de su implementación.
struct vfs_fs_ops *tarfs_get_vfs_ops(void);

#endif