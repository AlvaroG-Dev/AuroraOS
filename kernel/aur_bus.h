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

#endif