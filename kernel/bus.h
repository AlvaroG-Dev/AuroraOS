// kernel/bus.h
#ifndef KERNEL_BUS_H
#define KERNEL_BUS_H

#include "driver.h"
#include "spinlock.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Abstracción genérica de bus. Un bus contiene devices y drivers.
//
//   device_register(dev)   -> añade dev a bus->devices, intenta probe
//                             contra cada driver registrado.
//   bus_add_driver(drv)    -> añade drv a bus->drivers, intenta probe
//                             contra cada device registrado.
//   drivers_probe_all()    -> barre todos los buses y prueba todo lo no
//                             reclamado (idempotente; llamable desde .kur).
//
// Un driver puede ser:
//   - bus-managed: .bus != NULL. Vive en bus->drivers. probe/remove.
//   - legacy:      .bus == NULL. Vive en drivers[] de driver.c.
//                   init/shutdown. Sin cambios respecto a v2.
// ---------------------------------------------------------------------------

struct bus_type;
struct driver;

struct device {
  char name[16];              // "0000:00:1f.2" para PCI
  struct bus_type *bus;
  void *priv;                 // bus-specific (pci_device_t* para PCI)
  const void *id;             // ID propio del device (bus-specific, opcional)
  const void *matched_id;     // entrada de id_table del driver que matcheó
  struct driver *driver;      // driver que lo ha reclamado, NULL si libre
  struct device *next;        // siguiente en bus->devices
};

struct bus_type {
  const char *name;

  // Devuelve 1 si el device matchea algún id del driver.
  // Si match, escribe el id en *id_out (para que el probe lo reciba).
  int (*match)(struct device *dev, struct driver *drv, const void **id_out);

  // Llama a dev->driver->probe(dev). Debe respetar la convención de errno.
  int (*probe)(struct device *dev);

  // Opcional: llama a dev->driver->remove(dev).
  void (*remove)(struct device *dev);

  struct device *devices;     // lista LIFO de devices
  struct driver *drivers;     // lista LIFO de drivers
  int device_count;
  int driver_count;
  spinlock_t lock;            // protege devices y drivers
};

// Registra un bus. Idempotente: llamarlo dos veces no hace nada.
int bus_register(struct bus_type *bus);
void bus_unregister(struct bus_type *bus);

// Añade un device al bus y prueba contra los drivers ya registrados.
// Devuelve 0 si el device quedó registrado (haya o no probe OK).
int device_register(struct device *dev);
void device_unregister(struct device *dev);

// Añade un driver al bus y prueba contra los devices ya registrados.
int bus_add_driver(struct driver *drv);
// Quita el driver del bus y desvincula (llamando a bus->remove) los devices
// que tuviera reclamados.
void bus_del_driver(struct driver *drv);

// Barrido global: todos los buses, devices no reclamados contra todos
// los drivers. Idempotente. Útil tras cargar .kur en Fase 3.3.
void drivers_probe_all(void);

#endif