// kernel/kmod_sys.h
//
// [FASE 3.4] Interfaz Linux para módulos de kernel (.kur): syscalls
// init_module / finit_module / delete_module y las vistas /proc/modules y
// /sys/module/<m>/ que usa kmod (modprobe, rmmod, lsmod, insmod).
#ifndef KERNEL_KMOD_SYS_H
#define KERNEL_KMOD_SYS_H

#include <stddef.h>
#include <stdint.h>

int64_t k_init_module(uint64_t umod, uint64_t len, uint64_t uargs, uint64_t a4,
                      uint64_t a5);
int64_t k_finit_module(uint64_t fd, uint64_t uargs, uint64_t flags,
                       uint64_t a4, uint64_t a5);
int64_t k_delete_module(uint64_t uname, uint64_t flags, uint64_t a3,
                        uint64_t a4, uint64_t a5);

// Contenido de /proc/modules (kmalloc, NUL-terminado). NULL si no hay memoria.
char *kmod_gen_proc_modules(size_t *out_len);

// /sys/module/<name>/<attr>. Devuelve un buffer kmalloc o NULL si el módulo
// no existe. Atributos: "initstate", "refcnt", "taint".
char *kmod_gen_sysfs_attr(const char *module, const char *attr,
                          size_t *out_len);

// Nombre del módulo número `index` (0..n-1 entre los cargados) o NULL.
const char *kmod_loaded_name(uint64_t index);

// Nombre del `index`-ésimo módulo que importa símbolos de `module`
// ("holders"), o NULL.
const char *kmod_holder_name(const char *module, uint64_t index);

// 1 si `name` es un módulo cargado.
int kmod_is_loaded(const char *name);

#define KMOD_NAME_MAX 64

#endif
