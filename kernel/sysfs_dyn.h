// kernel/sysfs_dyn.h
//
// Registro dinamico de nodos de /sys (ficheros de texto y symlinks) para los
// subsistemas que exponen dispositivos al estilo Linux: input, DRM... Los
// directorios existen mientras haya algo debajo. Rutas relativas a /sys, sin
// barra inicial: "devices/virtual/input/input0/name".

#ifndef KERNEL_SYSFS_DYN_H
#define KERNEL_SYSFS_DYN_H

#include "vfs.h"
#include <stddef.h>

#define SYSFS_DYN_NONE 0
#define SYSFS_DYN_FILE 1
#define SYSFS_DYN_LINK 2
#define SYSFS_DYN_DIR 3

// Crea o reemplaza un fichero de texto (el contenido se copia).
int sysfs_dyn_set_file(const char *path, const char *content);
int sysfs_dyn_add_symlink(const char *path, const char *target);
int sysfs_dyn_remove_tree(const char *prefix); // quita path y todo lo de debajo

// Devuelve el tipo de `path` y, para FILE/LINK, una copia kmalloc'd del
// contenido/destino (el llamante hace kfree) y su longitud.
int sysfs_dyn_get(const char *path, char **data, size_t *len);

// readdir de `dir` ("" = /sys). Al final: 0 con out->name[0]==0.
// -ENOENT si no existe como directorio dinamico.
int sysfs_dyn_readdir(const char *dir, uint64_t index, vfs_dirent_t *out);

#endif
