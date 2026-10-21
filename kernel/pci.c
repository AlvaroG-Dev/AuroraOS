// kernel/pci.c
//
// Enumeración de dispositivos PCI + bus driver framework.
//
// [FASE 2.1b] Añade:
//   - spinlock global para config space (fix de corrupción paralela).
//   - Tabla de drivers PCI (pci_bus_register_driver / pci_bus_probe_all).
//   - Caché de dispositivos enumerados (pci_get_devices).

#include "pci.h"
#include "io.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"

// ---------------------------------------------------------------------------
// [FIX 2.1b] Lock global para config space. NO coger nunca mientras se
// mantiene otro lock, excepto sched_lock (nunca se coge desde aquí).
// ---------------------------------------------------------------------------
static spinlock_t pci_config_lock;

// ---------------------------------------------------------------------------
// [FASE 2.1b] Cache de dispositivos + tabla de drivers PCI.
// ---------------------------------------------------------------------------
#define PCI_MAX_DEVICES 64
#define PCI_MAX_DRIVERS 16

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static int g_pci_device_count = 0;

static pci_driver_t *g_pci_drivers[PCI_MAX_DRIVERS];
static int g_pci_driver_count = 0;

// ---------------------------------------------------------------------------
// Acceso config space. `__pci_*` son las versiones SIN lock (llamante
// debe tener pci_config_lock). Los públicos son wrappers que toman el
// lock por operación atómica.
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
// Nombres legibles de clases (sin cambios).
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
// pci_get_device: toma el lock para toda la lectura del dispositivo.
// Esto garantiza consistencia entre vendor, device, class, BARs, etc.
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
// Utilidades (usan los wrappers públicos, ya atómicos).
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
// Dump (sin cambios).
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

// ---------------------------------------------------------------------------
// [FASE 2.1b] Matching y dispatch de drivers PCI.
// ---------------------------------------------------------------------------
static int pci_id_match(const pci_device_t *dev, const pci_device_id_t *id) {
  if (!id || id->vendor_id == 0)
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

void pci_bus_register_driver(pci_driver_t *drv) {
  if (!drv || !drv->id_table || !drv->probe)
    return;
  if (g_pci_driver_count >= PCI_MAX_DRIVERS) {
    LOG_ERR("[PCI] pci_bus_register_driver: tabla de drivers llena");
    return;
  }
  g_pci_drivers[g_pci_driver_count++] = drv;
  LOG_INFO("[PCI] driver PCI registrado: '%s'", drv->name);
}

void pci_bus_probe_all(void) {
  if (g_pci_driver_count == 0 || g_pci_device_count == 0) {
    LOG_INFO("[PCI] probe: %d drivers, %d devices; nada que hacer",
             g_pci_driver_count, g_pci_device_count);
    return;
  }
  LOG_INFO("[PCI] probing %d driver(s) sobre %d device(s)", g_pci_driver_count,
           g_pci_device_count);

  for (int i = 0; i < g_pci_device_count; i++) {
    pci_device_t *dev = &g_pci_devices[i];
    int claimed = 0;
    for (int j = 0; j < g_pci_driver_count && !claimed; j++) {
      pci_driver_t *drv = g_pci_drivers[j];
      for (int k = 0; !claimed; k++) {
        const pci_device_id_t *id = &drv->id_table[k];
        if (id->vendor_id == 0)
          break; // terminator
        if (!pci_id_match(dev, id))
          continue;
        int rc = drv->probe(dev, id);
        if (rc == 0) {
          LOG_INFO("[PCI] %02x:%02x.%x claimed by '%s'", dev->bus, dev->slot,
                   dev->func, drv->name);
          claimed = 1;
        }
      }
    }
  }
}

const pci_device_t *pci_get_devices(int *count) {
  if (count)
    *count = g_pci_device_count;
  return g_pci_devices;
}

// ---------------------------------------------------------------------------
// pci_init_impl: scan principal. Guarda copia de cada dispositivo en
// g_pci_devices y, al final, dispara pci_bus_probe_all().
// ---------------------------------------------------------------------------
static int pci_init_impl(void) {
  LOG_INFO("[PCI] Iniciando enumeracion de dispositivos...");
  spin_init(&pci_config_lock);
  g_pci_device_count = 0;

  int count = 0;
  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      uint16_t vendor = pci_read_config_word((uint8_t)bus, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;
      uint8_t header = pci_read_config_byte((uint8_t)bus, slot, 0, 0x0E);
      int num_funcs = (header & 0x80) ? 8 : 1;
      for (uint8_t func = 0; func < num_funcs; func++) {
        pci_device_t dev;
        if (pci_get_device((uint8_t)bus, slot, func, &dev) != 0)
          continue;
        pci_dump_device(&dev);
        count++;
        if (g_pci_device_count < PCI_MAX_DEVICES)
          g_pci_devices[g_pci_device_count++] = dev;
      }
    }
  }

  LOG_INFO("[PCI] Enumeracion completada (%d dispositivos)", count);

  // [FASE 2.1b] Disparar el probe de drivers registrados.
  pci_bus_probe_all();
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
  uint8_t cap = pci_find_capability(bus, slot, func, 0x05);
  if (!cap)
    return;
  uint16_t msg_ctrl = pci_read_config_word(bus, slot, func, cap + 2);
  msg_ctrl &= ~0x0001;
  pci_write_config_word(bus, slot, func, cap + 2, msg_ctrl);
}

// ---------------------------------------------------------------------------
// Driver (struct driver, no pci_driver_t): sólo hace el scan principal.
// ---------------------------------------------------------------------------
struct driver pci_driver = {
    .name = "pci",
    .depends = NULL,
    .init = pci_init_impl,
    .shutdown = NULL,
};