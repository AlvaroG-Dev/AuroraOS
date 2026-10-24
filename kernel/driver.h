// kernel/driver.h (v3)
#ifndef KERNEL_DRIVER_H
#define KERNEL_DRIVER_H

#include <stdint.h>

struct bus_type;
struct device;

typedef const char *const *driver_depends_t;

struct driver {
  const char *name;

  // -----------------------------------------------------------------------
  // v3: bus-managed driver. Si bus != NULL, el driver se registra en el
  // bus (bus_add_driver) y su probe se llama cuando un device matchea.
  // .depends/.init se ignoran en este modo.
  // -----------------------------------------------------------------------
  struct bus_type *bus;
  const void *id_table;       // bus-specific, terminado en zero
  int (*probe)(struct device *dev);  // dev->matched_id tiene el id que matcheó
  void (*remove)(struct device *dev);
  struct driver *next_on_bus; // interno (lista del bus)

  // -----------------------------------------------------------------------
  // v2 legacy: init/shutdown global, .depends para orden topológico.
  // Solo se usa si bus == NULL.
  // -----------------------------------------------------------------------
  driver_depends_t depends;
  int (*init)(void);
  void (*shutdown)(void);
};

void driver_register(struct driver *d);

int drivers_init_all(void);
int drivers_init_all_parallel(void);
void drivers_shutdown_all(void);

#endif