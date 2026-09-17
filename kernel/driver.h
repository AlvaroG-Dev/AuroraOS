// kernel/driver.h
#ifndef KERNEL_DRIVER_H
#define KERNEL_DRIVER_H

struct driver {
  const char *name;
  int (*init)(void);
  void (*shutdown)(void);
};

// Registra un driver. Debe llamarse ANTES de drivers_init_all.
void driver_register(struct driver *d);

// Inicializa todos los drivers registrados, en orden.
// Devuelve 0 si todos OK, -1 si alguno falló.
int drivers_init_all(void);

// Apaga todos los drivers en orden inverso.
void drivers_shutdown_all(void);

#endif