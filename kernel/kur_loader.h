// kernel/kur_loader.h
#ifndef KERNEL_KUR_LOADER_H
#define KERNEL_KUR_LOADER_H

#include "kur.h"
#include <stddef.h>
#include <stdint.h>

struct kur_module {
  char     name[KUR_MAX_NAME];
  uint64_t base_va;
  uint64_t base_size;
  void    *init_fn;
  void    *exit_fn;
  uint64_t fini_array;   // .fini_array del módulo (0 si no tiene)
  uint64_t fini_count;
  int      index;
  int      in_use;

  // Refcounts. `n_dependents`: cuántos símbolos exportados por este módulo
  // están siendo usados por otros módulos. Si > 0, unload devuelve -EBUSY.
  uint32_t n_dependents;

  // `n_imports_from[b]`: cuántos símbolos de g_modules[b] usa este módulo.
  // En unload se resta de g_modules[b].n_dependents.
  uint32_t n_imports_from[KUR_MAX_MODULES];
};

void kur_loader_init(void);

int kur_loader_load_buffer(const void *data, size_t size,
                           struct kur_module **out_module);

int kur_loader_unload(struct kur_module *mod);

int kur_loader_count(void);
struct kur_module *kur_loader_get(int index);
struct kur_module *kur_loader_find_by_name(const char *name);

// Introspección de la tabla dinámica de símbolos (para /proc y tests).
int kur_dynsym_count(void);
// Dirección de un símbolo exportado por un módulo cargado (0 si no existe).
uint64_t kur_loader_lookup_symbol(const char *name);

// ---------------------------------------------------------------------------
// [FASE 3.3] Carga desde VFS.
//
// kur_loader_load_file: lee `path` con vfs_read_all(), lo pasa a
// kur_loader_load_buffer() y libera el buffer. Mismo contrato de
// errores que load_buffer (-ENOENT si no existe, -ENOEXEC si el .kur
// es inválido, etc.).
//
// kur_load_all: escanea `dir` (típicamente "/lib/modules"), filtra
// ficheros "*.kur", los ordena alfabéticamente (para respetar
// dependencias hello2 -> hello de forma determinista) y carga uno a
// uno.
// Devuelve:
//   >= 0: número de módulos cargados correctamente
//   <  0: -errno si `dir` no existe / no es directorio / readdir falla
// Los fallos individuales de un .kur se loguean pero NO abortan el
// bucle (un módulo roto no impide cargar los siguientes).
// ---------------------------------------------------------------------------
int kur_loader_load_file(const char *path, struct kur_module **out_module);
int kur_load_all(const char *dir);

#endif  // KERNEL_KUR_LOADER_H