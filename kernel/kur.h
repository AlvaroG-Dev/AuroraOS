// kernel/kur.h
#ifndef KERNEL_KUR_H
#define KERNEL_KUR_H

#include <stddef.h>
#include <stdint.h>

#define KUR_MAGIC       0x4B555221u  // "KUR!" little-endian
#define KUR_ABI_VERSION 2u            // v2: + SHA-256 en el manifiesto

#define KUR_MODULE_BASE 0xFFFFFFFFB0000000ULL

// Tamaño máximo del fichero .kur. kur_loader_load_file() lo lee entero con
// kmalloc, cuyo tope por asignación es 64 MiB (heap.c). Módulos mayores
// (p. ej. nvidia.ko con el núcleo binario) necesitarán leerse por trozos.
#define KUR_MAX_SIZE     (64u * 1024 * 1024)
#define KUR_MAX_MODULES  32
#define KUR_MAX_NAME     64

#define KUR_SYM_INIT     "kur_init"
#define KUR_SYM_EXIT     "kur_exit"
#define KUR_SYM_MANIFEST "__kur_manifest"

// Manifest flags
#define KUR_FLAG_HASHED  0x0001u  // hash[] contiene SHA-256 válido

struct kur_manifest {
  uint32_t magic;                // KUR_MAGIC
  uint16_t abi_version;          // KUR_ABI_VERSION
  uint16_t flags;                // KUR_FLAG_*
  uint32_t name_len;             // 1..63
  uint32_t reserved;             // 0
  char     name[KUR_MAX_NAME];   // NUL-terminated
  uint8_t  hash[32];             // SHA-256 con hash[32] y flags fijos
};

// ---------------------------------------------------------------------------
// Exports entre módulos. Un .kur puede exportar símbolos para que otros .kur
// los importen. El loader recorre `.kur_ksymtab` tras aplicar las
// relocaciones, y registra cada entrada en la tabla dinámica de símbolos.
//
// Uso en el .kur:
//   int hello_add(int, int) { ... }
//   KUR_EXPORT_SYMBOL(hello_add);
//
// El otro .kur:
//   extern int hello_add(int, int);
//
// Nota: este macro NO sustituye a KEXPORT_SYMBOL de kernel/ksymtab.h (ese
// exporta el símbolo al espacio de módulos desde el kernel). Aquí es para
// símbolos que viven DENTRO de un módulo.
// ---------------------------------------------------------------------------
struct kur_export_entry {
  const char *name;   // resuelto por el loader a base + rodata
  uint64_t    addr;   // resuelto por el loader a la VA del símbolo
};

#define KUR_EXPORT_SECTION ".kur_ksymtab"

#define KUR_EXPORT_SYMBOL(sym)                                          \
  static const struct kur_export_entry __kur_exp_##sym                  \
      __attribute__((used, section(KUR_EXPORT_SECTION))) = {            \
          #sym, (uint64_t)(uintptr_t)(&(sym))}

#endif  // KERNEL_KUR_H