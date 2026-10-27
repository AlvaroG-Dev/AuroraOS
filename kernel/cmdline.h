// kernel/cmdline.h
//
// Linea de comandos del kernel: la clave cmdline= de aurora.conf, que el
// bootloader (UEFI o BIOS) deja en kernel_boot_info. Opciones separadas por
// espacios, "clave=valor" o solo "clave".

#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <stddef.h>

// Copia la linea del bootloader (se llama una vez, en kmain).
void cmdline_init(const char *s);
// La linea completa ("" si el bootloader no paso ninguna).
const char *kernel_cmdline(void);

// Valor de `key` en la linea del kernel: 1 si esta (out recibe el valor, ""
// si es una opcion sin '='), 0 si no. Si se repite, gana la ultima.
int cmdline_get(const char *key, char *out, size_t outlen);
// Lo mismo sobre una linea cualquiera (para las pruebas).
int cmdline_find(const char *line, const char *key, char *out, size_t outlen);

#endif
