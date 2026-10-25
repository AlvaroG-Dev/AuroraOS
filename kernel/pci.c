// kernel/pci.c
//
// Enumeración de dispositivos PCI + bus_type PCI.
//
// [FASE 2.1b] Cambios respecto a la entrega anterior:
//   - pci_driver_t y pci_bus_register_driver/pci_bus_probe_all ELIMINADOS.
//   - pci_bus es ahora un struct bus_type genérico (bus.h).
//   - Cada dispositivo enumerado crea un struct device con:
//       name  = "0000:BB:SS.F"
//       priv  = &g_pci_devices[idx]  (pci_device_t*)
//       id    = &g_pci_devices[idx]
//     y se registra con device_register() para que el bus haga el probe.
//   - pci_id_match() pasa a ser pública (la necesita pci_bus_match).
//   - pci_init_impl() llama a bus_register(&pci_bus) (idempotente) y
//     drivers_probe_all() al final (red de seguridad).
//
// No se modifica la enumeración en sí ni el acceso a config space.

#include "pci.h"
#include "bus.h"
#include "io.h"
#include "klog.h"
#include "paging.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"

// ---------------------------------------------------------------------------
// Lock global para config space.
// ---------------------------------------------------------------------------
static spinlock_t pci_config_lock;

// ---------------------------------------------------------------------------
// Cache de dispositivos enumerados.
// ---------------------------------------------------------------------------
#define PCI_MAX_DEVICES 64

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static int g_pci_device_count = 0;

// struct device paralelo: g_pci_dev_nodes[i] <-> g_pci_devices[i].
// Almacenamiento estático (no kmalloc) para que el ciclo de vida sea
// trivial: la lista del bus apunta a estos nodos y nunca se liberan.
static struct device g_pci_dev_nodes[PCI_MAX_DEVICES];

// ---------------------------------------------------------------------------
// Acceso config space. `__pci_*` son las versiones SIN lock; los públicos
// son wrappers que toman el lock por operación atómica.
// ---------------------------------------------------------------------------
static inline uint32_t __pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func,
                                        uint8_t offset) {
  outl(PCI_CONFIG_ADDRESS, PCI_MAKE_ADDRESS(bus, slot, func, offset));
  return inl(PCI_CONFIG_DATA);
}
static inline uint16_t __pci_read_word(uint8_t bus, uint8_t slot, uint8_t func,
                                       uint8_t offset) {
  outl(PCI_CONFIG_ADDRESS, PCI_MAKE_ADDRESS(bus, slot, func, offset));
  uint32_t v = inl(PCI_CONFIG_DATA);
  return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}
static inline uint8_t __pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func,
                                      uint8_t offset) {
  outl(PCI_CONFIG_ADDRESS, PCI_MAKE_ADDRESS(bus, slot, func, offset));
  uint32_t v = inl(PCI_CONFIG_DATA);
  return (uint8_t)((v >> ((offset & 3) * 8)) & 0xFF);
}
static inline void __pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func,
                                     uint8_t offset, uint32_t value) {
  outl(PCI_CONFIG_ADDRESS, PCI_MAKE_ADDRESS(bus, slot, func, offset));
  outl(PCI_CONFIG_DATA, value);
}
static inline void __pci_write_word(uint8_t bus, uint8_t slot, uint8_t func,
                                    uint8_t offset, uint16_t value) {
  outl(PCI_CONFIG_ADDRESS, PCI_MAKE_ADDRESS(bus, slot, func, offset));
  uint32_t old = inl(PCI_CONFIG_DATA);
  uint32_t shift = (offset & 2) * 8;
  uint32_t mask = 0xFFFFU << shift;
  outl(PCI_CONFIG_DATA, (old & ~mask) | ((uint32_t)value << shift));
}
static inline void __pci_write_byte(uint8_t bus, uint8_t slot, uint8_t func,
                                    uint8_t offset, uint8_t value) {
  outl(PCI_CONFIG_ADDRESS, PCI_MAKE_ADDRESS(bus, slot, func, offset));
  uint32_t old = inl(PCI_CONFIG_DATA);
  uint32_t shift = (offset & 3) * 8;
  uint32_t mask = 0xFFU << shift;
  outl(PCI_CONFIG_DATA, (old & ~mask) | ((uint32_t)value << shift));
}

// ---------------------------------------------------------------------------
// API pública. Cada llamada es atómica.
// ---------------------------------------------------------------------------
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t offset) {
  unsigned long flags = spin_lock_irqsave(&pci_config_lock);
  uint32_t v = __pci_read_dword(bus, slot, func, offset);
  spin_unlock_irqrestore(&pci_config_lock, flags);
  return v;
}
uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t offset) {
  unsigned long flags = spin_lock_irqsave(&pci_config_lock);
  uint16_t v = __pci_read_word(bus, slot, func, offset);
  spin_unlock_irqrestore(&pci_config_lock, flags);
  return v;
}
uint8_t pci_read_config_byte(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t offset) {
  unsigned long flags = spin_lock_irqsave(&pci_config_lock);
  uint8_t v = __pci_read_byte(bus, slot, func, offset);
  spin_unlock_irqrestore(&pci_config_lock, flags);
  return v;
}
void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset, uint32_t value) {
  unsigned long flags = spin_lock_irqsave(&pci_config_lock);
  __pci_write_dword(bus, slot, func, offset, value);
  spin_unlock_irqrestore(&pci_config_lock, flags);
}
void pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset, uint16_t value) {
  unsigned long flags = spin_lock_irqsave(&pci_config_lock);
  __pci_write_word(bus, slot, func, offset, value);
  spin_unlock_irqrestore(&pci_config_lock, flags);
}
void pci_write_config_byte(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset, uint8_t value) {
  unsigned long flags = spin_lock_irqsave(&pci_config_lock);
  __pci_write_byte(bus, slot, func, offset, value);
  spin_unlock_irqrestore(&pci_config_lock, flags);
}

// ---------------------------------------------------------------------------
// Nombres legibles de clases.
// ---------------------------------------------------------------------------
static const char *pci_class_name(uint8_t class_id) {
  switch (class_id) {
  case PCI_CLASS_UNCLASSIFIED:
    return "Unclassified";
  case PCI_CLASS_STORAGE:
    return "Mass Storage";
  case PCI_CLASS_NETWORK:
    return "Network";
  case PCI_CLASS_DISPLAY:
    return "Display";
  case PCI_CLASS_MULTIMEDIA:
    return "Multimedia";
  case PCI_CLASS_MEMORY:
    return "Memory";
  case PCI_CLASS_BRIDGE:
    return "Bridge";
  case PCI_CLASS_COMMUNICATION:
    return "Communication";
  case PCI_CLASS_GENERIC_SYSTEM:
    return "System";
  case PCI_CLASS_INPUT:
    return "Input";
  case PCI_CLASS_DOCKING:
    return "Docking";
  case PCI_CLASS_PROCESSOR:
    return "Processor";
  case PCI_CLASS_SERIAL_BUS:
    return "Serial Bus";
  case PCI_CLASS_WIRELESS:
    return "Wireless";
  case PCI_CLASS_INTELLIGENT_IO:
    return "Intelligent IO";
  case PCI_CLASS_SATELLITE:
    return "Satellite";
  case PCI_CLASS_ENCRYPTION:
    return "Encryption";
  case PCI_CLASS_SIGNAL_PROCESSING:
    return "Signal Proc";
  case PCI_CLASS_PROCESSING_ACCEL:
    return "Proc Accel";
  case PCI_CLASS_NON_ESSENTIAL:
    return "Non-Essential";
  default:
    return "Unknown";
  }
}
static const char *pci_storage_subclass_name(uint8_t sub) {
  switch (sub) {
  case PCI_SUBCLASS_STORAGE_SCSI:
    return "SCSI";
  case PCI_SUBCLASS_STORAGE_IDE:
    return "IDE";
  case PCI_SUBCLASS_STORAGE_FLOPPY:
    return "Floppy";
  case PCI_SUBCLASS_STORAGE_IPI:
    return "IPI";
  case PCI_SUBCLASS_STORAGE_RAID:
    return "RAID";
  case PCI_SUBCLASS_STORAGE_ATA:
    return "ATA";
  case PCI_SUBCLASS_STORAGE_SATA:
    return "SATA";
  case PCI_SUBCLASS_STORAGE_SAS:
    return "SAS";
  case PCI_SUBCLASS_STORAGE_NVME:
    return "NVMe";
  case PCI_SUBCLASS_STORAGE_OTHER:
    return "Other";
  default:
    return "?";
  }
}
static const char *pci_bridge_subclass_name(uint8_t sub) {
  switch (sub) {
  case PCI_SUBCLASS_BRIDGE_HOST:
    return "Host";
  case PCI_SUBCLASS_BRIDGE_ISA:
    return "ISA";
  case PCI_SUBCLASS_BRIDGE_EISA:
    return "EISA";
  case PCI_SUBCLASS_BRIDGE_MCA:
    return "MCA";
  case PCI_SUBCLASS_BRIDGE_PCI:
    return "PCI-to-PCI";
  case PCI_SUBCLASS_BRIDGE_PCMCIA:
    return "PCMCIA";
  case PCI_SUBCLASS_BRIDGE_NUBUS:
    return "NuBus";
  case PCI_SUBCLASS_BRIDGE_CARDBUS:
    return "CardBus";
  default:
    return "?";
  }
}

// ---------------------------------------------------------------------------
// Lectura de BARs. Asume lock cogido.
// ---------------------------------------------------------------------------
static void pci_read_bars_locked(pci_device_t *dev) {
  memset(dev->bar, 0, sizeof(dev->bar));
  memset(dev->bar_is_io, 0, sizeof(dev->bar_is_io));
  memset(dev->bar_is_64, 0, sizeof(dev->bar_is_64));

  for (int i = 0; i < 6; i++) {
    uint32_t bar =
        __pci_read_dword(dev->bus, dev->slot, dev->func, 0x10 + i * 4);
    if (bar == 0)
      continue;
    if (bar & 0x01) {
      dev->bar_is_io[i] = 1;
      dev->bar[i] = bar & ~0x03U;
    } else {
      uint8_t type = (bar >> 1) & 0x03;
      if (type == 0x02) {
        dev->bar_is_64[i] = 1;
        uint32_t bar_hi = __pci_read_dword(dev->bus, dev->slot, dev->func,
                                           0x10 + (i + 1) * 4);
        dev->bar[i] = (bar & ~0x0FU) | ((uint64_t)bar_hi << 32);
        i++;
      } else {
        dev->bar[i] = bar & ~0x0FU;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// pci_get_device: atómico por dispositivo.
// ---------------------------------------------------------------------------
int pci_get_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t *dev) {
  if (!dev)
    return -1;

  memset(dev, 0, sizeof(*dev));

  unsigned long flags = spin_lock_irqsave(&pci_config_lock);

  uint16_t vendor = __pci_read_word(bus, slot, func, 0x00);
  if (vendor == 0xFFFF) {
    spin_unlock_irqrestore(&pci_config_lock, flags);
    return -1;
  }

  dev->bus = bus;
  dev->slot = slot;
  dev->func = func;
  dev->vendor_id = vendor;
  dev->device_id = __pci_read_word(bus, slot, func, 0x02);
  dev->revision_id = __pci_read_byte(bus, slot, func, 0x08);
  dev->prog_if = __pci_read_byte(bus, slot, func, 0x09);

  uint16_t class_reg = __pci_read_word(bus, slot, func, 0x0A);
  dev->class_id = (uint8_t)((class_reg >> 8) & 0xFF);
  dev->subclass_id = (uint8_t)(class_reg & 0xFF);

  dev->header_type = __pci_read_byte(bus, slot, func, 0x0E) & 0x7F;
  dev->multifunction = (__pci_read_byte(bus, slot, func, 0x0E) & 0x80) ? 1 : 0;

  dev->irq_line = __pci_read_byte(bus, slot, func, 0x3C);
  dev->irq_pin = __pci_read_byte(bus, slot, func, 0x3D);

  if (dev->header_type == PCI_HEADER_TYPE_DEVICE) {
    pci_read_bars_locked(dev);
  } else if (dev->header_type == PCI_HEADER_TYPE_BRIDGE) {
    pci_read_bars_locked(dev);
    dev->secondary_bus = __pci_read_byte(bus, slot, func, 0x19);
    dev->subordinate_bus = __pci_read_byte(bus, slot, func, 0x1A);
  }

  if (dev->header_type == PCI_HEADER_TYPE_DEVICE) {
    dev->subsystem_vendor_id = __pci_read_word(bus, slot, func, 0x2C);
    dev->subsystem_id = __pci_read_word(bus, slot, func, 0x2E);
  }

  spin_unlock_irqrestore(&pci_config_lock, flags);
  return 0;
}

// ---------------------------------------------------------------------------
// Búsqueda (usa pci_get_device que ya es atómico por dispositivo).
// ---------------------------------------------------------------------------
static int pci_match_class(const pci_device_t *dev, uint8_t class_id,
                           uint8_t subclass_id) {
  if (dev->class_id != class_id)
    return 0;
  if (subclass_id != 0xFF && dev->subclass_id != subclass_id)
    return 0;
  return 1;
}

int pci_find_device(uint8_t class_id, uint8_t subclass_id, pci_device_t *dev) {
  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      uint16_t vendor = pci_read_config_word((uint8_t)bus, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;
      uint8_t header = pci_read_config_byte((uint8_t)bus, slot, 0, 0x0E);
      int num_funcs = (header & 0x80) ? 8 : 1;
      for (uint8_t func = 0; func < num_funcs; func++) {
        pci_device_t tmp;
        if (pci_get_device((uint8_t)bus, slot, func, &tmp) != 0)
          continue;
        if (pci_match_class(&tmp, class_id, subclass_id)) {
          *dev = tmp;
          return 0;
        }
      }
    }
  }
  return -1;
}

int pci_find_next_device(uint8_t class_id, uint8_t subclass_id,
                         uint8_t *start_bus, uint8_t *start_slot,
                         uint8_t *start_func, pci_device_t *dev) {
  for (uint16_t bus = *start_bus; bus < 256; bus++) {
    uint8_t slot_start = (bus == *start_bus) ? *start_slot : 0;
    for (uint8_t slot = slot_start; slot < 32; slot++) {
      uint16_t vendor = pci_read_config_word((uint8_t)bus, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;
      uint8_t header = pci_read_config_byte((uint8_t)bus, slot, 0, 0x0E);
      int num_funcs = (header & 0x80) ? 8 : 1;
      uint8_t func_start =
          (bus == *start_bus && slot == *start_slot) ? *start_func + 1 : 0;
      for (uint8_t func = func_start; func < num_funcs; func++) {
        pci_device_t tmp;
        if (pci_get_device((uint8_t)bus, slot, func, &tmp) != 0)
          continue;
        if (pci_match_class(&tmp, class_id, subclass_id)) {
          *dev = tmp;
          *start_bus = (uint8_t)bus;
          *start_slot = slot;
          *start_func = func;
          return 0;
        }
      }
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Utilidades.
// ---------------------------------------------------------------------------
uint32_t pci_get_bar(const pci_device_t *dev, int bar_num) {
  if (!dev || bar_num < 0 || bar_num > 5)
    return 0;
  return dev->bar[bar_num];
}
uint32_t pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func, int bar_num) {
  pci_device_t dev;
  if (pci_get_device(bus, slot, func, &dev) != 0)
    return 0;
  return pci_get_bar(&dev, bar_num);
}
void pci_enable_bus_mastering(const pci_device_t *dev) {
  if (!dev)
    return;
  uint16_t cmd = pci_read_config_word(dev->bus, dev->slot, dev->func, 0x04);
  cmd |= PCI_CMD_BUS_MASTER;
  cmd &= ~PCI_CMD_INT_DISABLE;
  pci_write_config_word(dev->bus, dev->slot, dev->func, 0x04, cmd);
  LOG_INFO("[PCI] %02x:%02x.%x: bus master enabled, cmd=0x%04x", dev->bus,
           dev->slot, dev->func, cmd);
}
void pci_enable_io_mem(const pci_device_t *dev) {
  if (!dev)
    return;
  uint16_t cmd = pci_read_config_word(dev->bus, dev->slot, dev->func, 0x04);
  cmd |= PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE;
  pci_write_config_word(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
uint8_t pci_get_irq(const pci_device_t *dev) {
  if (!dev)
    return 0xFF;
  return dev->irq_line;
}

// ---------------------------------------------------------------------------
// Dump.
// ---------------------------------------------------------------------------
static void pci_dump_device(const pci_device_t *dev) {
  const char *class_name = pci_class_name(dev->class_id);
  const char *subclass_name = "";
  if (dev->class_id == PCI_CLASS_STORAGE)
    subclass_name = pci_storage_subclass_name(dev->subclass_id);
  else if (dev->class_id == PCI_CLASS_BRIDGE)
    subclass_name = pci_bridge_subclass_name(dev->subclass_id);

  LOG_INFO("[PCI] %02x:%02x.%x vendor=0x%04x device=0x%04x | %s %s "
           "(class=0x%02x sub=0x%02x progif=0x%02x)",
           dev->bus, dev->slot, dev->func, dev->vendor_id, dev->device_id,
           class_name, subclass_name, dev->class_id, dev->subclass_id,
           dev->prog_if);

  for (int i = 0; i < 6; i++) {
    if (dev->bar[i] == 0)
      continue;
    LOG_INFO("[PCI]   BAR%d: %s 0x%x%s", i, dev->bar_is_io[i] ? "I/O" : "MMIO",
             dev->bar[i], dev->bar_is_64[i] ? " (64-bit)" : "");
  }
  if (dev->class_id == PCI_CLASS_BRIDGE &&
      dev->subclass_id == PCI_SUBCLASS_BRIDGE_PCI) {
    LOG_INFO("[PCI]   Bridge: secondary=%u subordinate=%u", dev->secondary_bus,
             dev->subordinate_bus);
  }
}

// ===========================================================================
// [FASE 2.1b] Bus PCI genérico.
//
// pci_bus es un struct bus_type que conecta los drivers PCI con la capa
// genérica de bus.c. Match: compara dev->priv (pci_device_t) contra cada
// entrada del id_table del driver. Probe: delega en dev->driver->probe.
// ===========================================================================

// Matching público. vendor_id==0 es el terminador de la id_table:
// el llamador (pci_bus_match) NO lo pasa, pero mantenemos la guarda
// por si un .kur la llama directamente con una entrada terminador.
int pci_id_match(const pci_device_t *dev, const pci_device_id_t *id) {
  if (!dev || !id || id->vendor_id == 0)
    return 0;
  if (id->vendor_id != PCI_ANY_ID && id->vendor_id != dev->vendor_id)
    return 0;
  if (id->device_id != PCI_ANY_ID && id->device_id != dev->device_id)
    return 0;
  if (id->subvendor != PCI_ANY_ID && id->subvendor != dev->subsystem_vendor_id)
    return 0;
  if (id->subdevice != PCI_ANY_ID && id->subdevice != dev->subsystem_id)
    return 0;
  if (id->class_mask != 0) {
    if ((dev->class_id & id->class_mask) != (id->class_id & id->class_mask))
      return 0;
    if ((dev->subclass_id & id->class_mask) !=
        (id->subclass_id & id->class_mask))
      return 0;
  }
  return 1;
}

static int pci_bus_match(struct device *dev, struct driver *drv,
                         const void **id_out) {
  if (!drv->id_table)
    return 0;
  const pci_device_id_t *ids = (const pci_device_id_t *)drv->id_table;
  const pci_device_t *pdev = (const pci_device_t *)dev->priv;
  if (!pdev)
    return 0;

  for (int i = 0; ids[i].vendor_id != 0; i++) {
    if (pci_id_match(pdev, &ids[i])) {
      if (id_out)
        *id_out = &ids[i];
      return 1;
    }
  }
  return 0;
}

static int pci_bus_probe(struct device *dev) {
  if (!dev->driver || !dev->driver->probe)
    return -EINVAL;
  return dev->driver->probe(dev);
}

static void pci_bus_remove(struct device *dev);

struct bus_type pci_bus = {
    .name = "pci",
    .match = pci_bus_match,
    .probe = pci_bus_probe,
    .remove = pci_bus_remove,
};

// ---------------------------------------------------------------------------
// Nombre de device: "0000:BB:SS.F". Estático, no formatea (evita snprintf
// que no tenemos en el kernel).
// ---------------------------------------------------------------------------
static void pci_format_name(struct device *d, const pci_device_t *p) {
  static const char hex[] = "0123456789abcdef";
  d->name[0] = '0';
  d->name[1] = '0';
  d->name[2] = '0';
  d->name[3] = '0';
  d->name[4] = ':';
  d->name[5] = hex[(p->bus >> 4) & 0xF];
  d->name[6] = hex[p->bus & 0xF];
  d->name[7] = ':';
  d->name[8] = hex[(p->slot >> 4) & 0xF];
  d->name[9] = hex[p->slot & 0xF];
  d->name[10] = '.';
  d->name[11] = hex[p->func & 0xF];
  d->name[12] = '\0';
}
// ===========================================================================
// [FASE 6.4] Hot-plug PCI.
//
// Un kthread (pci-hotplug) reexplora los buses cada PCI_HOTPLUG_PERIOD_MS y
// compara con la cache:
//   - dispositivo nuevo: se le asignan los BARs que el firmware dejó a 0 (un
//     dispositivo enchufado en caliente llega con los BARs reseteados) desde
//     la ventana de su puente, se refresca sysfs y se registra en el bus, lo
//     que dispara el probe (driver de kernel o aur_bus -> .aur).
//   - dispositivo desaparecido: device_unregister() avisa al driver
//     (aur_bus mata el .aur) y se libera su hueco.
// Es sondeo, no el evento de slot de PCIe (pciehp): vale en cualquier
// plataforma y no depende de ACPI, a costa de ~1 s de latencia.
// ===========================================================================
#include "kthread.h"
#include "wait.h"

#define PCI_HOTPLUG_PERIOD_MS 1000u

extern void sysfs_pci_refresh(int max_bus);

static uint8_t g_pci_present[PCI_MAX_DEVICES]; // 1 = el dispositivo está en el bus
static int g_pci_max_bus;                      // último bus a explorar
static int g_rescanning;                       // un solo rescan a la vez

int pci_max_bus(void) { return g_pci_max_bus; }

static int pci_cache_find(uint8_t bus, uint8_t slot, uint8_t func) {
  for (int i = 0; i < g_pci_device_count; i++) {
    const pci_device_t *d = &g_pci_devices[i];
    if (g_pci_present[i] && d->bus == bus && d->slot == slot && d->func == func)
      return i;
  }
  return -1;
}

// Recalcula el último bus a explorar: el mayor `subordinate` de los puentes
// presentes (o 0 si no hay).
static void pci_update_max_bus(void) {
  int mb = 0;
  for (int i = 0; i < g_pci_device_count; i++) {
    const pci_device_t *d = &g_pci_devices[i];
    if (g_pci_present[i] && d->header_type == PCI_HEADER_TYPE_BRIDGE &&
        d->subordinate_bus > mb)
      mb = d->subordinate_bus;
  }
  g_pci_max_bus = mb;
}

// ---------------------------------------------------------------------------
// Ventanas de puente y asignación de BARs
// ---------------------------------------------------------------------------
// Ventana MMIO del puente cuyo bus secundario es `bus`. prefetch=1: la
// prefetchable (hasta 64 bits). Devuelve 0 y [*base,*limit] (inclusive), o -1
// si no hay puente o la ventana está cerrada (base > limit).
static int pci_bridge_window(uint8_t bus, int prefetch, uint64_t *base,
                             uint64_t *limit) {
  for (int i = 0; i < g_pci_device_count; i++) {
    const pci_device_t *br = &g_pci_devices[i];
    if (!g_pci_present[i] || br->header_type != PCI_HEADER_TYPE_BRIDGE ||
        br->secondary_bus != bus)
      continue;
    uint8_t b = br->bus, s = br->slot, f = br->func;
    uint64_t lo, hi;
    uint16_t rb = pci_read_config_word(b, s, f, prefetch ? 0x24 : 0x20);
    uint16_t rl = pci_read_config_word(b, s, f, prefetch ? 0x26 : 0x22);
    lo = (uint64_t)(rb & 0xFFF0) << 16;
    hi = ((uint64_t)(rl & 0xFFF0) << 16) | 0xFFFFF;
    if (prefetch && (rb & 0xF) == 1) { // ventana de 64 bits
      lo |= (uint64_t)pci_read_config_dword(b, s, f, 0x28) << 32;
      hi |= (uint64_t)pci_read_config_dword(b, s, f, 0x2C) << 32;
    }
    if (lo > hi)
      return -1;
    *base = lo;
    *limit = hi;
    return 0;
  }
  return -1;
}

struct pci_bar_info {
  uint64_t addr, size;
  int is_io, is_64, prefetch;
};

// Mide un BAR escribiendo 1s y restaurándolo. Devuelve cuántos registros
// ocupa (1 o 2); size=0 si el BAR no existe. El llamante decide si
// deshabilitar la decodificación antes.
static int pci_bar_probe(const pci_device_t *d, int i, struct pci_bar_info *o) {
  uint8_t b = d->bus, s = d->slot, f = d->func, off = (uint8_t)(0x10 + i * 4);
  memset(o, 0, sizeof(*o));
  uint32_t orig = pci_read_config_dword(b, s, f, off);
  if (orig & 1) {
    o->is_io = 1;
    pci_write_config_dword(b, s, f, off, 0xFFFFFFFF);
    uint32_t m = pci_read_config_dword(b, s, f, off);
    pci_write_config_dword(b, s, f, off, orig);
    o->addr = orig & ~0x3u;
    o->size = (uint32_t)(-(int32_t)(m & ~0x3u)) & 0xFFFF;
    return 1;
  }
  o->is_64 = ((orig >> 1) & 3) == 2;
  o->prefetch = (orig >> 3) & 1;
  pci_write_config_dword(b, s, f, off, 0xFFFFFFFF);
  uint32_t m = pci_read_config_dword(b, s, f, off);
  pci_write_config_dword(b, s, f, off, orig);
  o->addr = orig & ~0xFu;
  uint32_t lo = (uint32_t)(-(int32_t)(m & ~0xFu));
  if (!o->is_64) {
    o->size = lo;
    return 1;
  }
  uint32_t orig_hi = pci_read_config_dword(b, s, f, off + 4);
  pci_write_config_dword(b, s, f, off + 4, 0xFFFFFFFF);
  uint32_t mh = pci_read_config_dword(b, s, f, off + 4);
  pci_write_config_dword(b, s, f, off + 4, orig_hi);
  o->addr |= (uint64_t)orig_hi << 32;
  if (lo)
    o->size = lo;
  else if (mh)
    o->size = ((uint64_t)(uint32_t)(~mh) + 1) << 32;
  else
    o->size = 0;
  return 2;
}

// Primera dirección libre de la ventana: justo después del BAR más alto que
// ya haya dentro de ella entre los dispositivos presentes de ese bus.
static uint64_t pci_window_cursor(uint8_t bus, int prefetch, uint64_t base,
                                  uint64_t limit, const pci_device_t *skip) {
  uint64_t cur = base;
  for (int i = 0; i < g_pci_device_count; i++) {
    const pci_device_t *d = &g_pci_devices[i];
    if (!g_pci_present[i] || d == skip || d->bus != bus ||
        d->header_type != PCI_HEADER_TYPE_DEVICE)
      continue;
    for (int b = 0; b < 6;) {
      struct pci_bar_info bi;
      int n = pci_bar_probe(d, b, &bi);
      if (!bi.is_io && bi.size && bi.addr >= base && bi.addr <= limit &&
          bi.prefetch == prefetch && bi.addr + bi.size > cur)
        cur = bi.addr + bi.size;
      b += n;
    }
  }
  return cur;
}

// Asigna los BARs MMIO a 0 de un dispositivo nuevo desde la ventana de su
// puente. Los BARs de E/S no se asignan. Deja la decodificación como estaba:
// la enciende el driver (pci_enable_io_mem).
static void pci_assign_bars(pci_device_t *d) {
  if (d->header_type != PCI_HEADER_TYPE_DEVICE || d->bus == 0)
    return; // en el bus 0 no hay puente del que repartir
  uint16_t cmd = pci_read_config_word(d->bus, d->slot, d->func, 0x04);
  pci_write_config_word(d->bus, d->slot, d->func, 0x04,
                        cmd & ~(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE));

  uint64_t placed_end[2] = {0, 0}; // fin de lo ya colocado a este dispositivo
  for (int i = 0; i < 6;) {
    struct pci_bar_info bi;
    int n = pci_bar_probe(d, i, &bi);
    int cur_i = i;
    i += n;
    if (bi.is_io || bi.size == 0 || bi.addr != 0)
      continue;
    uint64_t base, limit;
    if (pci_bridge_window(d->bus, bi.prefetch, &base, &limit) != 0) {
      LOG_WARN("[PCI-HP] %02x:%02x.%x BAR%d (%lu B): ventana %s del puente "
               "cerrada (¿falta mem-reserve?)",
               d->bus, d->slot, d->func, cur_i, (unsigned long)bi.size,
               bi.prefetch ? "prefetchable" : "MMIO");
      continue;
    }
    uint64_t cursor = pci_window_cursor(d->bus, bi.prefetch, base, limit, d);
    if (placed_end[bi.prefetch] > cursor)
      cursor = placed_end[bi.prefetch];
    uint64_t addr = (cursor + bi.size - 1) & ~(bi.size - 1);
    if (addr + bi.size - 1 > limit) {
      LOG_WARN("[PCI-HP] %02x:%02x.%x BAR%d: sin hueco en la ventana "
               "[0x%lx,0x%lx]",
               d->bus, d->slot, d->func, cur_i, (unsigned long)base,
               (unsigned long)limit);
      continue;
    }
    uint8_t off = (uint8_t)(0x10 + cur_i * 4);
    uint32_t flags = pci_read_config_dword(d->bus, d->slot, d->func, off) & 0xF;
    pci_write_config_dword(d->bus, d->slot, d->func, off,
                           (uint32_t)addr | flags);
    if (bi.is_64)
      pci_write_config_dword(d->bus, d->slot, d->func, (uint8_t)(off + 4),
                             (uint32_t)(addr >> 32));
    placed_end[bi.prefetch] = addr + bi.size;
    LOG_INFO("[PCI-HP] %02x:%02x.%x BAR%d -> 0x%lx (%lu B%s)", d->bus, d->slot,
             d->func, cur_i, (unsigned long)addr, (unsigned long)bi.size,
             bi.prefetch ? ", pref" : "");
  }

  // Releer los BARs en la ficha.
  pci_device_t fresh;
  if (pci_get_device(d->bus, d->slot, d->func, &fresh) == 0) {
    memcpy(d->bar, fresh.bar, sizeof(d->bar));
    memcpy(d->bar_is_io, fresh.bar_is_io, sizeof(d->bar_is_io));
    memcpy(d->bar_is_64, fresh.bar_is_64, sizeof(d->bar_is_64));
  }
  pci_write_config_word(d->bus, d->slot, d->func, 0x04, cmd);
}

// ---------------------------------------------------------------------------
// Rescan
// ---------------------------------------------------------------------------
static void pci_bus_remove(struct device *dev) {
  if (dev->driver && dev->driver->remove)
    dev->driver->remove(dev);
}

// ---------------------------------------------------------------------------
// Eventos de slot de PCIe (pciehp mínimo)
//
// En PCIe un dispositivo no desaparece solo: pedir su retirada (botón de
// atención / device_del) solo avisa, y es el SO quien debe apagar el slot.
// Y un dispositivo recién enchufado puede tener el slot sin alimentar. Se
// atiende desde el sondeo, sin interrupciones de slot.
// ---------------------------------------------------------------------------
#define PCIE_CAP_ID 0x10
#define PCIE_SLOT_IMPLEMENTED (1u << 8) // PCIe Capabilities
#define PCIE_SLTCTL 0x18
#define PCIE_SLTSTA 0x1A
#define PCIE_SLTSTA_ABP (1u << 0) // Attention Button Pressed
#define PCIE_SLTSTA_PDC (1u << 3) // Presence Detect Changed
#define PCIE_SLTSTA_PDS (1u << 6) // Presence Detect State
#define PCIE_SLTCTL_PCC (1u << 10) // Power Controller Control: 1 = apagado
#define PCIE_SLTCTL_PIC_MASK (3u << 8)
#define PCIE_SLTCTL_PIC_ON (1u << 8)
#define PCIE_SLTCTL_PIC_OFF (3u << 8)
#define PCIE_SLTCTL_AIC_OFF (3u << 6)

static void pci_slot_events(void) {
  for (int i = 0; i < g_pci_device_count; i++) {
    const pci_device_t *br = &g_pci_devices[i];
    if (!g_pci_present[i] || br->header_type != PCI_HEADER_TYPE_BRIDGE)
      continue;
    uint8_t b = br->bus, s = br->slot, f = br->func;
    uint8_t cap = pci_find_capability(b, s, f, PCIE_CAP_ID);
    if (!cap || !(pci_read_config_word(b, s, f, cap + 2) & PCIE_SLOT_IMPLEMENTED))
      continue;

    uint16_t sta = pci_read_config_word(b, s, f, cap + PCIE_SLTSTA);
    uint16_t ctl = pci_read_config_word(b, s, f, cap + PCIE_SLTCTL);
    if (sta & PCIE_SLTSTA_PDC) {
      // Inserción (o retirada ya consumada). Al enchufar, QEMU/el chasis puede
      // levantar también el botón de atención: no es una petición de retirada.
      pci_write_config_word(b, s, f, cap + PCIE_SLTSTA,
                            PCIE_SLTSTA_PDC | PCIE_SLTSTA_ABP);
      if ((sta & PCIE_SLTSTA_PDS) && (ctl & PCIE_SLTCTL_PCC)) {
        ctl = (uint16_t)((ctl & ~(PCIE_SLTCTL_PIC_MASK | PCIE_SLTCTL_PCC)) |
                         PCIE_SLTCTL_PIC_ON);
        pci_write_config_word(b, s, f, cap + PCIE_SLTCTL, ctl);
        LOG_INFO("[PCI-HP] %02x:%02x.%x: dispositivo presente; slot alimentado",
                 b, s, f);
      }
    } else if (sta & PCIE_SLTSTA_ABP) {
      // Petición de retirada: apagar el slot. El dispositivo desaparece del
      // bus y el siguiente barrido lo da de baja.
      pci_write_config_word(b, s, f, cap + PCIE_SLTSTA, PCIE_SLTSTA_ABP);
      ctl = (uint16_t)((ctl & ~PCIE_SLTCTL_PIC_MASK) | PCIE_SLTCTL_PCC |
                       PCIE_SLTCTL_PIC_OFF | PCIE_SLTCTL_AIC_OFF);
      pci_write_config_word(b, s, f, cap + PCIE_SLTCTL, ctl);
      LOG_INFO("[PCI-HP] %02x:%02x.%x: retirada pedida; slot apagado", b, s, f);
    }
  }
}

// Reexplora el bus. Devuelve el nº de cambios (altas + bajas).
int pci_rescan(void) {
  if (__atomic_exchange_n(&g_rescanning, 1, __ATOMIC_ACQ_REL))
    return 0;

  pci_slot_events();

  uint8_t seen[PCI_MAX_DEVICES];
  memset(seen, 0, sizeof(seen));
  int added_idx[PCI_MAX_DEVICES], n_added = 0, changes = 0;

  for (int b = 0; b <= g_pci_max_bus; b++) {
    for (int slot = 0; slot < 32; slot++) {
      if (pci_read_config_word((uint8_t)b, (uint8_t)slot, 0, 0) == 0xFFFF)
        continue;
      uint8_t header =
          pci_read_config_byte((uint8_t)b, (uint8_t)slot, 0, 0x0E);
      int nf = (header & 0x80) ? 8 : 1;
      for (int func = 0; func < nf; func++) {
        pci_device_t dev;
        if (pci_get_device((uint8_t)b, (uint8_t)slot, (uint8_t)func, &dev) !=
            0)
          continue;
        int idx = pci_cache_find((uint8_t)b, (uint8_t)slot, (uint8_t)func);
        if (idx >= 0) {
          seen[idx] = 1;
          continue;
        }
        // Nuevo: hueco libre (reutiliza bajas) o al final.
        int slot_idx = -1;
        for (int i = 0; i < g_pci_device_count; i++)
          if (!g_pci_present[i]) {
            slot_idx = i;
            break;
          }
        if (slot_idx < 0) {
          if (g_pci_device_count >= PCI_MAX_DEVICES) {
            LOG_WARN("[PCI-HP] cache llena, ignorando %02x:%02x.%x", b, slot,
                     func);
            continue;
          }
          slot_idx = g_pci_device_count;
        }
        pci_assign_bars(&dev);
        g_pci_devices[slot_idx] = dev;
        struct device *node = &g_pci_dev_nodes[slot_idx];
        memset(node, 0, sizeof(*node));
        pci_format_name(node, &dev);
        node->bus = &pci_bus;
        node->priv = &g_pci_devices[slot_idx];
        node->id = &g_pci_devices[slot_idx];
        if (slot_idx == g_pci_device_count)
          g_pci_device_count++;
        g_pci_present[slot_idx] = 1;
        seen[slot_idx] = 1;
        added_idx[n_added++] = slot_idx;
        LOG_INFO("[PCI-HP] alta %s vendor=0x%04x device=0x%04x", node->name,
                 dev.vendor_id, dev.device_id);
        changes++;
      }
    }
  }

  // Bajas: presentes en la cache y no vistos.
  for (int i = 0; i < g_pci_device_count; i++) {
    if (!g_pci_present[i] || seen[i])
      continue;
    struct device *node = &g_pci_dev_nodes[i];
    LOG_INFO("[PCI-HP] baja %s", node->name);
    device_unregister(node);
    g_pci_present[i] = 0;
    changes++;
  }

  if (changes) {
    pci_update_max_bus();
    sysfs_pci_refresh(g_pci_max_bus);
    for (int k = 0; k < n_added; k++)
      device_register(&g_pci_dev_nodes[added_idx[k]]);
  }
  __atomic_store_n(&g_rescanning, 0, __ATOMIC_RELEASE);
  return changes;
}

static bool pci_hotplug_never(void *arg) {
  (void)arg;
  return false;
}

static wait_queue_t g_hp_wq;

static int pci_hotplug_thread(void *arg) {
  (void)arg;
  while (!kthread_should_stop()) {
    wait_event_timeout(&g_hp_wq, pci_hotplug_never, NULL,
                       PCI_HOTPLUG_PERIOD_MS);
    pci_rescan();
  }
  return 0;
}

static void pci_hotplug_start(void) {
  wait_queue_init(&g_hp_wq);
  pci_update_max_bus();
  sysfs_pci_refresh(g_pci_max_bus);
  if (!kthread_run(pci_hotplug_thread, NULL, "pci-hotplug"))
    LOG_WARN("[PCI-HP] no se pudo crear el kthread de hot-plug");
}


// ---------------------------------------------------------------------------
// pci_init_impl: scan principal. Crea un struct device por dispositivo
// y lo registra en pci_bus. device_register dispara bus_try_probe, que
// matchea contra los drivers ya registrados (ahci, xhci) y llama a su
// probe.
//
// Nota: device_register se llama sin tener pci_config_lock. Los probes
// (ahci_probe, xhci_probe_inner) pueden dormir/bloquear y a su vez
// llaman a pci_read_config_* que cogen el lock por operación. Sin
// anidamiento, sin deadlock.
// ---------------------------------------------------------------------------
static int pci_init_impl(void) {
  LOG_INFO("[PCI] Iniciando enumeracion de dispositivos...");
  spin_init(&pci_config_lock);
  g_pci_device_count = 0;

  // Registrar el bus PCI. Idempotente: drivers_init_task lo hace antes
  // para asegurarse de que existe antes de registrar drivers PCI.
  bus_register(&pci_bus);

  int count = 0;
  for (uint16_t b = 0; b < 256; b++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      uint16_t vendor = pci_read_config_word((uint8_t)b, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;
      uint8_t header = pci_read_config_byte((uint8_t)b, slot, 0, 0x0E);
      int num_funcs = (header & 0x80) ? 8 : 1;
      for (uint8_t func = 0; func < num_funcs; func++) {
        pci_device_t dev;
        if (pci_get_device((uint8_t)b, slot, func, &dev) != 0)
          continue;
        pci_dump_device(&dev);
        count++;

        if (g_pci_device_count >= PCI_MAX_DEVICES) {
          LOG_WARN("[PCI] cache de devices llena (%d), ignorando %02x:%02x.%x",
                   PCI_MAX_DEVICES, b, slot, func);
          continue;
        }

        int idx = g_pci_device_count;
        g_pci_devices[idx] = dev;

        struct device *node = &g_pci_dev_nodes[idx];
        memset(node, 0, sizeof(*node));
        pci_format_name(node, &dev);
        node->bus = &pci_bus;
        node->priv = &g_pci_devices[idx];
        node->id = &g_pci_devices[idx];
        node->driver = NULL;

        g_pci_device_count++;
        g_pci_present[idx] = 1;

        // Registro en el bus. Dispara bus_try_probe -> probe(dev) si
        // algún driver ya registrado matchea.
        device_register(node);
      }
    }
  }

  LOG_INFO("[PCI] Enumeracion completada (%d dispositivos, %d registrados)",
           count, g_pci_device_count);

  // Red de seguridad: probar todo lo que quede suelto. Cubre el caso
  // (improbable) de un driver que se registró DESPUÉS de un device en
  // otra CPU, o de un id_table que necesitó reintento.
  //
  // NOTA: drivers_probe_all() itera bus->devices sin lock. Seguro hoy
  // porque solo corre desde el kthread de pci_init_impl (single writer
  // durante el boot).
  drivers_probe_all();

  // [FASE 6.4] A partir de aquí se vigilan altas/bajas.
  pci_hotplug_start();

  return 0;
}

// ---------------------------------------------------------------------------
// Capability list y MSI.
// ---------------------------------------------------------------------------
uint8_t pci_find_capability(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t cap_id) {
  uint16_t status = pci_read_config_word(bus, slot, func, 0x06);
  if (!(status & PCI_STATUS_CAP_LIST))
    return 0;
  uint8_t cap_ptr = pci_read_config_byte(bus, slot, func, 0x34) & 0xFC;
  int guard = 0;
  while (cap_ptr != 0 && guard < 48) {
    uint8_t id = pci_read_config_byte(bus, slot, func, cap_ptr);
    if (id == cap_id)
      return cap_ptr;
    cap_ptr = pci_read_config_byte(bus, slot, func, cap_ptr + 1) & 0xFC;
    guard++;
  }
  return 0;
}

int pci_enable_msi(uint8_t bus, uint8_t slot, uint8_t func, uint8_t vector,
                   uint8_t lapic_id) {
  uint8_t cap = pci_find_capability(bus, slot, func, 0x05);
  if (!cap) {
    LOG_WARN("[PCI] %02x:%02x.%x: sin capability MSI", bus, slot, func);
    return -1;
  }
  uint16_t msg_ctrl = pci_read_config_word(bus, slot, func, cap + 2);
  int is_64 = (msg_ctrl >> 7) & 1;
  uint32_t addr_lo = 0xFEE00000U | ((uint32_t)lapic_id << 12);
  uint32_t addr_hi = 0;
  uint16_t data = vector;
  pci_write_config_dword(bus, slot, func, cap + 4, addr_lo);
  if (is_64)
    pci_write_config_dword(bus, slot, func, cap + 8, addr_hi);
  pci_write_config_word(bus, slot, func, cap + (is_64 ? 12 : 8), data);
  msg_ctrl |= 0x0001;
  pci_write_config_word(bus, slot, func, cap + 2, msg_ctrl);
  LOG_INFO("[PCI] %02x:%02x.%x: MSI habilitado (cap=0x%02x, vec=0x%02x, "
           "addr=0x%08x, 64bit=%d)",
           bus, slot, func, cap, vector, addr_lo, is_64);
  return 0;
}

void pci_disable_msi(uint8_t bus, uint8_t slot, uint8_t func) {
  // Libera también MSI-X si el dispositivo lo tenía activo (no-op si no).
  pci_disable_msix(bus, slot, func);
  uint8_t cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSI);
  if (!cap)
    return;
  uint16_t msg_ctrl = pci_read_config_word(bus, slot, func, cap + 2);
  msg_ctrl &= ~0x0001;
  pci_write_config_word(bus, slot, func, cap + 2, msg_ctrl);
}

// ---------------------------------------------------------------------------
// MSI-X.
//
// Capability (cap = offset en config space):
//   +2  Message Control: [10:0] tamaño-1, [14] function mask, [15] enable
//   +4  Table Offset / BIR: [2:0] BAR de la tabla, [31:3] offset en el BAR
//   +8  PBA Offset / BIR (no se usa: no hacemos polling de pendientes)
//
// Entrada de la tabla (16 bytes, MMIO):
//   +0 addr_lo  +4 addr_hi  +8 data  +12 vector control (bit0 = masked)
// ---------------------------------------------------------------------------
#define MSIX_CTRL_ENABLE (1u << 15)
#define MSIX_CTRL_FMASK (1u << 14)
#define MSIX_CTRL_TSIZE 0x07FFu
#define MSIX_ENTRY_SIZE 16u
#define MSIX_ENTRY_VCTRL 12u
#define MSIX_VCTRL_MASKED 1u
#define PCI_MSIX_MAX_DEVS 8

struct msix_state {
  uint8_t used;
  uint8_t bus, slot, func;
  uint8_t intx_was_enabled; // INTx lo apagamos al activar MSI-X; se restaura
  int count;
  volatile uint32_t *table;
};

static struct msix_state g_msix[PCI_MSIX_MAX_DEVS];
static spinlock_t g_msix_lock;

static struct msix_state *msix_find_locked(uint8_t bus, uint8_t slot,
                                           uint8_t func) {
  for (int i = 0; i < PCI_MSIX_MAX_DEVS; i++)
    if (g_msix[i].used && g_msix[i].bus == bus && g_msix[i].slot == slot &&
        g_msix[i].func == func)
      return &g_msix[i];
  return NULL;
}

static inline void msix_entry_write(volatile uint32_t *table, int entry,
                                    uint32_t addr_lo, uint32_t addr_hi,
                                    uint32_t data, uint32_t vctrl) {
  volatile uint32_t *e = table + (size_t)entry * (MSIX_ENTRY_SIZE / 4);
  e[0] = addr_lo;
  e[1] = addr_hi;
  e[2] = data;
  e[3] = vctrl;
}

int pci_msix_table_size(uint8_t bus, uint8_t slot, uint8_t func) {
  uint8_t cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
  if (!cap)
    return 0;
  return (pci_read_config_word(bus, slot, func, cap + 2) & MSIX_CTRL_TSIZE) + 1;
}

// Dirección física de la tabla MSI-X. 0 si el BAR no sirve. Se lee del config
// space porque pci_device_t.bar es de 32 bits y un BAR de 64 perdería la
// parte alta.
static uint64_t msix_table_phys(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t cap) {
  uint32_t tbl = pci_read_config_dword(bus, slot, func, cap + 4);
  uint8_t bir = tbl & 0x7;
  uint32_t off = tbl & ~0x7u;
  if (bir > 5)
    return 0;
  uint32_t bar = pci_read_config_dword(bus, slot, func, 0x10 + bir * 4);
  if (bar & 1) // BAR de I/O: no puede alojar la tabla
    return 0;
  uint64_t base = bar & ~0xFULL;
  if (((bar >> 1) & 0x3) == 0x2) {
    if (bir == 5)
      return 0;
    base |= (uint64_t)pci_read_config_dword(bus, slot, func,
                                            0x10 + (bir + 1) * 4)
            << 32;
  }
  if (base == 0)
    return 0;
  return base + off;
}

int pci_enable_msix(uint8_t bus, uint8_t slot, uint8_t func,
                    const uint8_t *vectors, int count, uint8_t lapic_id) {
  if (!vectors || count <= 0)
    return -1;
  uint8_t cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
  if (!cap)
    return -1;

  uint16_t ctrl = pci_read_config_word(bus, slot, func, cap + 2);
  int tsize = (ctrl & MSIX_CTRL_TSIZE) + 1;
  if (count > tsize) {
    LOG_WARN("[PCI] %02x:%02x.%x: MSI-X pide %d vectores, tabla de %d", bus,
             slot, func, count, tsize);
    return -1;
  }

  unsigned long fl = spin_lock_irqsave(&g_msix_lock);
  struct msix_state *st = msix_find_locked(bus, slot, func);
  if (!st) {
    for (int i = 0; i < PCI_MSIX_MAX_DEVS; i++) {
      if (!g_msix[i].used) {
        st = &g_msix[i];
        st->used = 1; // reservado; se libera abajo si algo falla
        st->bus = bus;
        st->slot = slot;
        st->func = func;
        st->table = NULL;
        st->count = 0;
        break;
      }
    }
  }
  spin_unlock_irqrestore(&g_msix_lock, fl);
  if (!st) {
    LOG_WARN("[PCI] tabla de MSI-X llena");
    return -1;
  }

  uint64_t phys = msix_table_phys(bus, slot, func, cap);
  void *mapped = NULL;
  if (phys)
    mapped = mmio_map(phys, (uint64_t)tsize * MSIX_ENTRY_SIZE,
                      PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!mapped) {
    LOG_WARN("[PCI] %02x:%02x.%x: no se pudo mapear la tabla MSI-X (phys=%p)",
             bus, slot, func, (void *)phys);
    st->used = 0;
    return -1;
  }
  volatile uint32_t *table = (volatile uint32_t *)mapped;

  // MSI y MSI-X son excluyentes.
  uint8_t msi_cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSI);
  if (msi_cap) {
    uint16_t mc = pci_read_config_word(bus, slot, func, msi_cap + 2);
    pci_write_config_word(bus, slot, func, msi_cap + 2, mc & ~0x0001);
  }

  // La tabla se escribe con MSI-X habilitado pero enmascarado a nivel
  // función, para que ninguna entrada a medio programar dispare.
  ctrl |= MSIX_CTRL_ENABLE | MSIX_CTRL_FMASK;
  pci_write_config_word(bus, slot, func, cap + 2, ctrl);

  // El acceso MMIO a la tabla exige Memory Space activo.
  uint16_t cmd = pci_read_config_word(bus, slot, func, 0x04);
  st->intx_was_enabled = !(cmd & PCI_CMD_INT_DISABLE);
  cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_INT_DISABLE;
  pci_write_config_word(bus, slot, func, 0x04, cmd);

  uint32_t addr_lo = 0xFEE00000U | ((uint32_t)lapic_id << 12);
  for (int i = 0; i < count; i++)
    msix_entry_write(table, i, addr_lo, 0, vectors[i], MSIX_VCTRL_MASKED);
  for (int i = 0; i < count; i++)
    table[(size_t)i * (MSIX_ENTRY_SIZE / 4) + MSIX_ENTRY_VCTRL / 4] = 0;
  (void)table[MSIX_ENTRY_VCTRL / 4]; // flush de escrituras posteadas

  ctrl &= ~MSIX_CTRL_FMASK;
  pci_write_config_word(bus, slot, func, cap + 2, ctrl);

  fl = spin_lock_irqsave(&g_msix_lock);
  st->table = table;
  st->count = count;
  spin_unlock_irqrestore(&g_msix_lock, fl);

  LOG_INFO("[PCI] %02x:%02x.%x: MSI-X habilitado (cap=0x%02x, %d/%d vectores, "
           "primero=0x%02x, tabla=%p)",
           bus, slot, func, cap, count, tsize, vectors[0], (void *)phys);
  return 0;
}

void pci_msix_mask_vector(uint8_t bus, uint8_t slot, uint8_t func, int entry,
                          int masked) {
  unsigned long fl = spin_lock_irqsave(&g_msix_lock);
  struct msix_state *st = msix_find_locked(bus, slot, func);
  if (st && st->table && entry >= 0 && entry < st->count) {
    volatile uint32_t *vc =
        st->table + (size_t)entry * (MSIX_ENTRY_SIZE / 4) +
        MSIX_ENTRY_VCTRL / 4;
    *vc = masked ? MSIX_VCTRL_MASKED : 0;
    (void)*vc;
  }
  spin_unlock_irqrestore(&g_msix_lock, fl);
}

void pci_disable_msix(uint8_t bus, uint8_t slot, uint8_t func) {
  unsigned long fl = spin_lock_irqsave(&g_msix_lock);
  struct msix_state *st = msix_find_locked(bus, slot, func);
  if (!st) {
    spin_unlock_irqrestore(&g_msix_lock, fl);
    return;
  }
  struct msix_state s = *st;
  st->used = 0;
  st->table = NULL;
  st->count = 0;
  spin_unlock_irqrestore(&g_msix_lock, fl);

  if (s.table)
    for (int i = 0; i < s.count; i++)
      s.table[(size_t)i * (MSIX_ENTRY_SIZE / 4) + MSIX_ENTRY_VCTRL / 4] =
          MSIX_VCTRL_MASKED;

  uint8_t cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
  if (cap) {
    uint16_t ctrl = pci_read_config_word(bus, slot, func, cap + 2);
    ctrl &= ~MSIX_CTRL_ENABLE;
    pci_write_config_word(bus, slot, func, cap + 2, ctrl);
  }
  if (s.intx_was_enabled) {
    uint16_t cmd = pci_read_config_word(bus, slot, func, 0x04);
    pci_write_config_word(bus, slot, func, 0x04, cmd & ~PCI_CMD_INT_DISABLE);
  }
}

int pci_has_msi_any(uint8_t bus, uint8_t slot, uint8_t func) {
  return pci_find_capability(bus, slot, func, PCI_CAP_ID_MSI) ||
         pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
}

// MSI de un solo vector; si el dispositivo solo tiene MSI-X, usa su entrada 0.
int pci_enable_msi_any(uint8_t bus, uint8_t slot, uint8_t func, uint8_t vector,
                       uint8_t lapic_id) {
  if (pci_find_capability(bus, slot, func, PCI_CAP_ID_MSI))
    return pci_enable_msi(bus, slot, func, vector, lapic_id);
  return pci_enable_msix(bus, slot, func, &vector, 1, lapic_id);
}

// ---------------------------------------------------------------------------
// Accessor público del cache de devices.
// ---------------------------------------------------------------------------
const pci_device_t *pci_get_devices(int *count) {
  if (count)
    *count = g_pci_device_count;
  return g_pci_devices;
}

// ---------------------------------------------------------------------------
// Driver legacy: solo hace el scan principal. Los drivers PCI (ahci,
// xhci) viven en pci_bus.drivers como bus-managed, no aquí.
// ---------------------------------------------------------------------------
struct driver pci_driver = {
    .name = "pci",
    .depends = NULL,
    .init = pci_init_impl,
    .shutdown = NULL,
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
#include "test.h"

// Sin cambios en el bus, un rescan no da altas ni bajas (y no toca la cache).
static void test_pci_rescan_idempotent(void) {
  int before = g_pci_device_count;
  TEST_ASSERT_EQ(pci_rescan(), 0);
  TEST_ASSERT_EQ(pci_rescan(), 0);
  TEST_ASSERT_EQ(g_pci_device_count, before);
  int present = 0;
  for (int i = 0; i < g_pci_device_count; i++)
    present += g_pci_present[i];
  TEST_ASSERT_EQ(present, before);
}
REGISTER_TEST("pci: rescan sin cambios es idempotente", test_pci_rescan_idempotent);

// El tamaño de tabla MSI-X reportado es coherente (1..2048) o 0 si no hay cap,
// y deshabilitar MSI-X en un dispositivo que no lo activó es inocuo.
static void test_pci_msix_table_size(void) {
  for (int i = 0; i < g_pci_device_count; i++) {
    const pci_device_t *d = &g_pci_devices[i];
    int n = pci_msix_table_size(d->bus, d->slot, d->func);
    int has = pci_find_capability(d->bus, d->slot, d->func, PCI_CAP_ID_MSIX) != 0;
    TEST_ASSERT(has ? (n >= 1 && n <= 2048) : n == 0, "tamaño tabla MSI-X");
    pci_disable_msix(d->bus, d->slot, d->func);
  }
  uint8_t v = 0x63;
  TEST_ASSERT(pci_enable_msix(0xFF, 0x1F, 7, &v, 1, 0) != 0,
              "MSI-X en dispositivo inexistente falla");
}
REGISTER_TEST("pci: tabla MSI-X coherente", test_pci_msix_table_size);
