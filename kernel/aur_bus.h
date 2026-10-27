// kernel/aur_bus.h
#ifndef KERNEL_AUR_BUS_H
#define KERNEL_AUR_BUS_H

#include "bus.h"

// Driver de bus que actúa como "fallback": si ningún driver del kernel
// reclama un dispositivo, busca un .aur en /usr/lib/aur/ que haga match
// con el manifiesto y lo lanza en espacio de usuario.
extern struct driver aur_bus_driver;

// [FASE 5.5] Lanza un .aur que no se asocia a un dispositivo PCI (p. ej.
// ps2.aur) y lo pone bajo el supervisor (reinicio automático). 0 = lanzado.
int aur_bus_launch(const char *path);

// Opcion de arranque aur.skip=VVVV:DDDD[,VVVV:DDDD...] (cmdline= de
// aurora.conf): aur_bus no reclama esos dispositivos aunque haya un .aur para
// ellos, y quedan para un driver del kernel (p. ej. uno de Linux sobre la capa
// LinuxKPI). DDDD puede ser '*' (todo el fabricante). Ejemplo, bochs y
// virtio-gpu para Linux: cmdline=aur.skip=1234:1111,1af4:1050
//
// aur_bus_skip_match() aplica una lista con ese formato (1 si casa).
int aur_bus_skip_match(const char *list, uint16_t vendor, uint16_t device);

#endif