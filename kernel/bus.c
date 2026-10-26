// kernel/bus.c
#include "bus.h"
#include "klog.h"
#include "uaccess.h"
#include "string.h"

#define MAX_BUSES 8
static struct bus_type *g_buses[MAX_BUSES];
static int g_bus_count = 0;

int bus_register(struct bus_type *bus) {
  if (!bus || !bus->name || !bus->match || !bus->probe)
    return -EINVAL;
  for (int i = 0; i < g_bus_count; i++)
    if (g_buses[i] == bus)
      return 0; // idempotente
  if (g_bus_count >= MAX_BUSES)
    return -ENOMEM;

  bus->devices = NULL;
  bus->drivers = NULL;
  bus->device_count = 0;
  bus->driver_count = 0;
  spin_init(&bus->lock);

  g_buses[g_bus_count++] = bus;
  LOG_INFO("[BUS] '%s' registrado", bus->name);
  return 0;
}

void bus_unregister(struct bus_type *bus) {
  for (int i = 0; i < g_bus_count; i++) {
    if (g_buses[i] == bus) {
      g_buses[i] = g_buses[--g_bus_count];
      return;
    }
  }
}

// Prueba un device contra todos los drivers del bus.
// Precondición: el device NO tiene driver asignado.
// Efecto: si un driver matchea y su probe devuelve 0, dev->driver queda
//         fijado. Si el probe falla, se sigue probando con el siguiente.
//
// NOTA: no se coge bus->lock durante el probe (puede dormir / bloquear).
// Esto es seguro en contexto de init / task (single-writer). Si en el
// futuro se necesita concurrencia, usar un flag atómico en dev.
static void bus_try_probe(struct device *dev) {
  struct bus_type *bus = dev->bus;
  if (dev->driver)
    return;

  for (struct driver *d = bus->drivers; d; d = d->next_on_bus) {
    const void *matched = NULL;
    if (!bus->match(dev, d, &matched))
      continue;

    dev->matched_id = matched;
    dev->driver = d;

    int rc = bus->probe(dev);
    if (rc == 0) {
      LOG_INFO("[BUS] %s: '%s' claimed by '%s'", bus->name, dev->name,
               d->name);
      return;
    }

    // -ENODEV = "este driver no es para este dispositivo": caso normal (p.ej.
    // aur_bus prueba todos los dispositivos PCI), no merece un WARN.
    if (rc == -ENODEV)
      LOG_DEBUG("[BUS] %s: '%s' rechazado por '%s'", bus->name, dev->name,
                d->name);
    else
      LOG_WARN("[BUS] %s: '%s' probe '%s' -> %d", bus->name, dev->name,
               d->name, rc);
    dev->driver = NULL;
  }
}

int device_register(struct device *dev) {
  if (!dev || !dev->bus || !dev->name[0])
    return -EINVAL;

  struct bus_type *bus = dev->bus;
  unsigned long flags = spin_lock_irqsave(&bus->lock);
  dev->next = bus->devices;
  bus->devices = dev;
  bus->device_count++;
  spin_unlock_irqrestore(&bus->lock, flags);

  LOG_DEBUG("[BUS] %s: device '%s' registrado (%d devices)", bus->name,
            dev->name, bus->device_count);

  bus_try_probe(dev);
  return 0;
}

void device_unregister(struct device *dev) {
  if (!dev || !dev->bus)
    return;
  struct bus_type *bus = dev->bus;

  // [FASE 6.4] Baja con driver: avisarle (sin bus lock: puede dormir). Solo
  // los buses que definen .remove (PCI) lo usan; el resto sigue como antes.
  if (dev->driver && bus->remove) {
    bus->remove(dev);
    dev->driver = NULL;
  }

  unsigned long flags = spin_lock_irqsave(&bus->lock);
  struct device **pp = &bus->devices;
  while (*pp) {
    if (*pp == dev) {
      *pp = dev->next;
      bus->device_count--;
      spin_unlock_irqrestore(&bus->lock, flags);
      return;
    }
    pp = &(*pp)->next;
  }
  spin_unlock_irqrestore(&bus->lock, flags);
}

int bus_add_driver(struct driver *drv) {
  if (!drv || !drv->bus || !drv->name)
    return -EINVAL;

  struct bus_type *bus = drv->bus;
  unsigned long flags = spin_lock_irqsave(&bus->lock);
  drv->next_on_bus = bus->drivers;
  bus->drivers = drv;
  bus->driver_count++;
  spin_unlock_irqrestore(&bus->lock, flags);

  LOG_INFO("[BUS] %s: driver '%s' registrado (%d drivers)", bus->name,
           drv->name, bus->driver_count);

  // Probar todos los devices no reclamados contra este driver nuevo.
  for (struct device *dev = bus->devices; dev; dev = dev->next)
    bus_try_probe(dev);

  return 0;
}

void bus_del_driver(struct driver *drv) {
  if (!drv || !drv->bus)
    return;
  struct bus_type *bus = drv->bus;
  unsigned long flags = spin_lock_irqsave(&bus->lock);
  struct driver **pp = &bus->drivers;
  while (*pp) {
    if (*pp == drv) {
      *pp = drv->next_on_bus;
      bus->driver_count--;
      break;
    }
    pp = &(*pp)->next_on_bus;
  }
  spin_unlock_irqrestore(&bus->lock, flags);

  // Desvincular los devices que reclamó: si no, quedarían apuntando a un
  // driver que puede estar a punto de descargarse (un .kur). Sin bus lock,
  // como device_unregister: remove puede dormir.
  for (struct device *dev = bus->devices; dev; dev = dev->next) {
    if (dev->driver != drv)
      continue;
    if (bus->remove)
      bus->remove(dev);
    dev->driver = NULL;
    dev->matched_id = NULL;
    LOG_INFO("[BUS] %s: '%s' liberado por '%s'", bus->name, dev->name,
             drv->name);
  }
}

void drivers_probe_all(void) {
  for (int i = 0; i < g_bus_count; i++) {
    struct bus_type *bus = g_buses[i];
    for (struct device *dev = bus->devices; dev; dev = dev->next)
      if (!dev->driver)
        bus_try_probe(dev);
  }
}