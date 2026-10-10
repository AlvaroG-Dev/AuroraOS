// kernel/tmpfs.h
#ifndef KERNEL_TMPFS_H
#define KERNEL_TMPFS_H

#include <stddef.h>
#include <stdint.h>

struct vfs_fs_ops;

// Crea una instancia de tmpfs con un limite de bytes. El puntero
// resultante se pasa a vfs_mount(path, tmpfs_get_vfs_ops(), priv).
// Devuelve NULL si no hay memoria.
void *tmpfs_new(size_t max_bytes);

// Libera una instancia creada con tmpfs_new. NO libera el arbol de
// nodos (asume que el FS vive hasta el final del boot). Se mejorara
// cuando se soporte umount real.
void tmpfs_destroy(void *priv);

// vfs_fs_ops para vfs_mount().
struct vfs_fs_ops *tmpfs_get_vfs_ops(void);

// Estadisticas: bytes usados actualmente.
size_t tmpfs_bytes_used(void *priv);

// Inicializa todas las instancias tmpfs del sistema: /etc, /tmp, /var,
// /run. Las monta con vfs_mount() y pre-popula /etc con los ficheros
// minimos que glibc espera (passwd, group, hosts, resolv.conf).
//
// Antes de shadow-ear /etc con el tmpfs, lee /etc/ld.so.cache de tarfs
// (generado en build time por ldconfig) y lo reescribe dentro del
// tmpfs. Sin esto, glibc pierde el cache y hace probing por cada lib.
//
// Llamar UNA VEZ en boot, DESPUES de vfs_init() (necesita la mount
// table inicializada) y ANTES de cargar init.
void tmpfs_init(void);

#endif