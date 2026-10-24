// kernel/ksymtab.h
#ifndef KERNEL_KSYMTAB_H
#define KERNEL_KSYMTAB_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Tabla de símbolos exportados por el kernel, para resolver imports de
// módulos .kur.
//
// Uso:
//   // En kernel/klog.c (o cualquier .c del kernel):
//   #include "ksymtab.h"
//   KEXPORT_SYMBOL(klog_printf);
//
//   // En el módulo:
//   extern void klog_printf(int, const char *, ...);
//   // El loader resuelve esta referencia contra ksymtab_lookup().
//
// El linker recoge todos los `KEXPORT_SYMBOL` en una sección `.ksymtab`
// (ver linker.ld.in). __ksymtab_start / __ksymtab_end los provee el
// linker script.
//
// Búsqueda lineal (10s de símbolos → ns). Añadir hash si algún día
// superamos los ~200 exports.
// ---------------------------------------------------------------------------

struct ksymtab_entry {
  const char *name;
  uint64_t    addr;
};

extern const struct ksymtab_entry __ksymtab_start[];
extern const struct ksymtab_entry __ksymtab_end[];

#define KEXPORT_SYMBOL(sym)                                            \
  static const struct ksymtab_entry __ksymtab_##sym                    \
      __attribute__((used, section(".ksymtab"))) = {                   \
          #sym, (uint64_t)(uintptr_t)(&(sym))}

// Inicializa la tabla (idempotente). Loguea el conteo. Llamar UNA vez
// en kmain, después de klog_init().
void ksymtab_init(void);

// Devuelve la dirección del símbolo, o 0 si no existe.
uint64_t ksymtab_lookup(const char *name);

size_t ksymtab_count(void);

#endif  // KERNEL_KSYMTAB_H