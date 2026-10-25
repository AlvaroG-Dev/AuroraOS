// kernel/devtmpfs.h
//
// Registro dinámico de nodos de dispositivo para /dev (el devfs de vfs.c solo
// conoce una tabla estática + los block devices). Los drivers (DRM, evdev,
// ...) registran aquí sus nodos con una ruta relativa a /dev ("dri/card0",
// "input/event0"); los subdirectorios existen mientras haya algo debajo.
// mknod(2) de un char device conocido crea un alias en este mismo registro.

#ifndef KERNEL_DEVTMPFS_H
#define KERNEL_DEVTMPFS_H

#include "vfs.h"
#include <stdbool.h>
#include <stdint.h>

#define DEVT_MKDEV(maj, min) (((uint32_t)(maj) << 8) | (uint32_t)(min))

// Registra un char device en /dev/<path>. `ops`/`priv` se copian al nodo en
// cada lookup (node->ops, node->priv). mode = permisos (sin S_IFCHR).
// 0 o -EEXIST / -ENOSPC / -EINVAL.
int devtmpfs_register(const char *path, uint32_t rdev, uint32_t mode,
                      uint32_t uid, uint32_t gid, vfs_ops_t *ops, void *priv);
int devtmpfs_unregister(const char *path); // 0 o -ENOENT

// Crea un nodo (kzalloc, ref_count 0, como los demás lookup del devfs) para
// la ruta relativa, o NULL. Si es un directorio implícito, ops = dir_ops.
vfs_node_t *devtmpfs_lookup(const char *rel, vfs_ops_t *dir_ops);
bool devtmpfs_exists(const char *rel);

// readdir del directorio `dir` ("" = raíz de /dev). Al terminar devuelve 0 con
// out->name[0] == 0. Devuelve -ENOENT si `dir` no es un directorio conocido.
int devtmpfs_readdir(const char *dir, uint64_t index, vfs_dirent_t *out);

// Busca un char device registrado por (major<<8|minor) para mknod.
bool devtmpfs_find_by_rdev(uint32_t rdev, vfs_ops_t **ops, void **priv);

#endif
