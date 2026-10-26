// kernel/sysfs_dyn.h
//
// Registro dinamico de nodos de /sys (ficheros de texto, atributos con
// callbacks y symlinks) para los subsistemas que exponen dispositivos al
// estilo Linux: input, DRM, la capa LinuxKPI... Los directorios existen
// mientras haya algo debajo. Rutas relativas a /sys, sin barra inicial:
// "devices/virtual/input/input0/name".

#ifndef KERNEL_SYSFS_DYN_H
#define KERNEL_SYSFS_DYN_H

#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

#define SYSFS_DYN_NONE 0
#define SYSFS_DYN_FILE 1
#define SYSFS_DYN_LINK 2
#define SYSFS_DYN_DIR 3
#define SYSFS_DYN_ATTR 4 // fichero cuyo contenido da un callback

#define SYSFS_DYN_PATH_MAX 192
#define SYSFS_DYN_ATTR_BUF 4096 // tamaño del buffer de show (PAGE_SIZE)

// Atributo con callbacks (como los de sysfs en Linux). show escribe el
// contenido en buf (SYSFS_DYN_ATTR_BUF bytes) y devuelve su longitud o
// -errno; store recibe lo escrito y devuelve los bytes consumidos o -errno
// (NULL = solo lectura). Se llaman sin cerrojos del registro tomados: pueden
// dormir. Tras sysfs_dyn_remove*() ya no hay ni habrá llamadas en curso.
typedef struct sysfs_dyn_attr_ops {
  long (*show)(void *ctx, char *buf, size_t size);
  long (*store)(void *ctx, const char *buf, size_t len);
} sysfs_dyn_attr_ops_t;

// Crea o reemplaza un fichero de texto (el contenido se copia).
int sysfs_dyn_set_file(const char *path, const char *content);
int sysfs_dyn_add_symlink(const char *path, const char *target);
// Crea un atributo; mode son los permisos (p. ej. 0444, 0644, 0200).
// -EEXIST si la ruta ya existe.
int sysfs_dyn_add_attr(const char *path, const sysfs_dyn_attr_ops_t *ops,
                       void *ctx, uint32_t mode);
int sysfs_dyn_remove(const char *path);       // solo esa entrada
int sysfs_dyn_remove_tree(const char *prefix); // quita path y todo lo de debajo

// Devuelve el tipo de `path` y, para FILE/LINK/ATTR, una copia kmalloc'd del
// contenido/destino (el llamante hace kfree) y su longitud. Para ATTR el
// contenido es el resultado de show en ese momento (vacio si no hay show).
int sysfs_dyn_get(const char *path, char **data, size_t *len);
// Permisos de `path` (0444 para ficheros y enlaces; el `mode` de un atributo).
uint32_t sysfs_dyn_mode(const char *path);
// Escribe en un atributo (llama a store). -EACCES si no tiene store.
long sysfs_dyn_store(const char *path, const char *buf, size_t len);

// readdir de `dir` ("" = /sys). Al final: 0 con out->name[0]==0.
// -ENOENT si no existe como directorio dinamico.
int sysfs_dyn_readdir(const char *dir, uint64_t index, vfs_dirent_t *out);

#endif
