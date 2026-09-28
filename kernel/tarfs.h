// kernel/tarfs.h
#ifndef TARFS_H
#define TARFS_H

#include <stddef.h>
#include <stdint.h>


typedef struct {
  char name[256]; // Ruta normalizada (ej: "system/icons/terminal.bmp")
  uint8_t *data;  // Puntero directo al inicio del contenido en RAM
  size_t size;    // Tamaño del archivo en bytes
  int is_dir;     // 1 = Directorio, 0 = Archivo
  // [SYMLINK] typeflag '2'. Si is_symlink=1, data/size son 0 y el
  // target está en linkname (relativo o absoluto).
  int is_symlink;
  char linkname[100];
} tar_node_t;

void tarfs_init(const void *tar_addr, size_t tar_size);
tar_node_t *tarfs_open(const char *path);
void tarfs_list(const char *dir_path);
size_t tarfs_get_node_count(void);
tar_node_t *tarfs_get_node(size_t index);
tar_node_t *tar_find_file(const char *path);

#endif