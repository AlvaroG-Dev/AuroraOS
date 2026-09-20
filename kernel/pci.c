// kernel/pci.c
//
// Enumeración de dispositivos PCI.
//
// Referencia: PCI Local Bus Specification 3.0.

#include "pci.h"
#include "io.h"
#include "klog.h"
#include "string.h"

// ===========================================================================
// Lectura/escritura de configuración PCI.
// ===========================================================================
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t offset) {
  uint32_t address = PCI_MAKE_ADDRESS(bus, slot, func, offset);
  outl(PCI_CONFIG_ADDRESS, address);
  return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t offset) {
  uint32_t address = PCI_MAKE_ADDRESS(bus, slot, func, offset);
  outl(PCI_CONFIG_ADDRESS, address);
  uint32_t val = inl(PCI_CONFIG_DATA);
  return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read_config_byte(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t offset) {
  uint32_t address = PCI_MAKE_ADDRESS(bus, slot, func, offset);
  outl(PCI_CONFIG_ADDRESS, address);
  uint32_t val = inl(PCI_CONFIG_DATA);
  return (uint8_t)((val >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset, uint32_t value) {
  uint32_t address = PCI_MAKE_ADDRESS(bus, slot, func, offset);
  outl(PCI_CONFIG_ADDRESS, address);
  outl(PCI_CONFIG_DATA, value);
}

void pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset, uint16_t value) {
  uint32_t address = PCI_MAKE_ADDRESS(bus, slot, func, offset);
  outl(PCI_CONFIG_ADDRESS, address);
  uint32_t old = inl(PCI_CONFIG_DATA);
  uint32_t shift = (offset & 2) * 8;
  uint32_t mask = 0xFFFFU << shift;
  uint32_t new_val = (old & ~mask) | ((uint32_t)value << shift);
  outl(PCI_CONFIG_DATA, new_val);
}

void pci_write_config_byte(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset, uint8_t value) {
  uint32_t address = PCI_MAKE_ADDRESS(bus, slot, func, offset);
  outl(PCI_CONFIG_ADDRESS, address);
  uint32_t old = inl(PCI_CONFIG_DATA);
  uint32_t shift = (offset & 3) * 8;
  uint32_t mask = 0xFFU << shift;
  uint32_t new_val = (old & ~mask) | ((uint32_t)value << shift);
  outl(PCI_CONFIG_DATA, new_val);
}

// ===========================================================================
// Nombres legibles de clases.
// ===========================================================================
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

// ===========================================================================
// Lectura de BARs.
//
// Para un BAR:
//   - Leer el valor.
//   - Si bit 0 == 1, es I/O BAR. La dirección es el valor & ~0x03.
//   - Si bit 0 == 0, es MMIO BAR. Puede ser de 64 bits (bits 1-2).
//     El tipo se determina por los bits 1-2:
//       0b00 = 32-bit MMIO
//       0b10 = 64-bit MMIO (usa el siguiente BAR para la parte alta)
//   - Si el valor es 0, el BAR no está implementado.
// ===========================================================================
static void pci_read_bars(pci_device_t *dev) {
  memset(dev->bar, 0, sizeof(dev->bar));
  memset(dev->bar_is_io, 0, sizeof(dev->bar_is_io));
  memset(dev->bar_is_64, 0, sizeof(dev->bar_is_64));

  for (int i = 0; i < 6; i++) {
    uint32_t bar =
        pci_read_config_dword(dev->bus, dev->slot, dev->func, 0x10 + i * 4);
    if (bar == 0)
      continue;

    if (bar & 0x01) {
      // I/O BAR.
      dev->bar_is_io[i] = 1;
      dev->bar[i] = bar & ~0x03U;
    } else {
      // MMIO BAR.
      uint8_t type = (bar >> 1) & 0x03;
      if (type == 0x02) {
        // 64-bit MMIO.
        dev->bar_is_64[i] = 1;
        uint32_t bar_hi = pci_read_config_dword(dev->bus, dev->slot, dev->func,
                                                0x10 + (i + 1) * 4);
        dev->bar[i] = (bar & ~0x0FU) | ((uint64_t)bar_hi << 32);
        i++; // Saltar el siguiente BAR (es la parte alta).
      } else {
        // 32-bit MMIO.
        dev->bar[i] = bar & ~0x0FU;
      }
    }
  }
}

// ===========================================================================
// Lectura completa de un dispositivo.
// ===========================================================================
int pci_get_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t *dev) {
  if (!dev)
    return -1;

  memset(dev, 0, sizeof(*dev));

  dev->bus = bus;
  dev->slot = slot;
  dev->func = func;

  uint16_t vendor = pci_read_config_word(bus, slot, func, 0x00);
  if (vendor == 0xFFFF)
    return -1;

  dev->vendor_id = vendor;
  dev->device_id = pci_read_config_word(bus, slot, func, 0x02);
  dev->revision_id = pci_read_config_byte(bus, slot, func, 0x08);
  dev->prog_if = pci_read_config_byte(bus, slot, func, 0x09);

  uint16_t class_reg = pci_read_config_word(bus, slot, func, 0x0A);
  dev->class_id = (uint8_t)((class_reg >> 8) & 0xFF);
  dev->subclass_id = (uint8_t)(class_reg & 0xFF);

  dev->header_type = pci_read_config_byte(bus, slot, func, 0x0E) & 0x7F;
  dev->multifunction =
      (pci_read_config_byte(bus, slot, func, 0x0E) & 0x80) ? 1 : 0;

  dev->irq_line = pci_read_config_byte(bus, slot, func, 0x3C);
  dev->irq_pin = pci_read_config_byte(bus, slot, func, 0x3D);

  // Leer BARs.
  if (dev->header_type == PCI_HEADER_TYPE_DEVICE) {
    pci_read_bars(dev);
  } else if (dev->header_type == PCI_HEADER_TYPE_BRIDGE) {
    pci_read_bars(
        dev); // Los bridges también tienen BARs (normalmente solo BAR0/1).
    // Leer el bus secundario y subordinado.
    dev->secondary_bus = pci_read_config_byte(bus, slot, func, 0x19);
    dev->subordinate_bus = pci_read_config_byte(bus, slot, func, 0x1A);
  }

  // Leer subsystem vendor/device (offset 0x2C, solo header type 0).
  if (dev->header_type == PCI_HEADER_TYPE_DEVICE) {
    dev->subsystem_vendor_id = pci_read_config_word(bus, slot, func, 0x2C);
    dev->subsystem_id = pci_read_config_word(bus, slot, func, 0x2E);
  }

  return 0;
}

// ===========================================================================
// Búsqueda de dispositivos.
// ===========================================================================
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
      // Verificar si el slot tiene un dispositivo.
      uint16_t vendor = pci_read_config_word((uint8_t)bus, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;

      // Comprobar multifunción.
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
  // Empezar desde el punto indicado.
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

// ===========================================================================
// Utilidades.
// ===========================================================================
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
  cmd &= ~PCI_CMD_INT_DISABLE; // Habilitar IRQs del dispositivo
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

// ===========================================================================
// Enumeración completa (para debug).
// ===========================================================================
static void pci_dump_device(const pci_device_t *dev) {
  const char *class_name = pci_class_name(dev->class_id);
  const char *subclass_name = "";

  if (dev->class_id == PCI_CLASS_STORAGE) {
    subclass_name = pci_storage_subclass_name(dev->subclass_id);
  } else if (dev->class_id == PCI_CLASS_BRIDGE) {
    subclass_name = pci_bridge_subclass_name(dev->subclass_id);
  }

  LOG_INFO("[PCI] %02x:%02x.%x vendor=0x%04x device=0x%04x | %s %s "
           "(class=0x%02x sub=0x%02x progif=0x%02x)",
           dev->bus, dev->slot, dev->func, dev->vendor_id, dev->device_id,
           class_name, subclass_name, dev->class_id, dev->subclass_id,
           dev->prog_if);

  // Mostrar BARs implementados.
  for (int i = 0; i < 6; i++) {
    if (dev->bar[i] == 0)
      continue;
    LOG_INFO("[PCI]   BAR%d: %s 0x%x%s", i, dev->bar_is_io[i] ? "I/O" : "MMIO",
             dev->bar[i], dev->bar_is_64[i] ? " (64-bit)" : "");
  }

  // Si es un bridge PCI-to-PCI, mostrar bus secundario.
  if (dev->class_id == PCI_CLASS_BRIDGE &&
      dev->subclass_id == PCI_SUBCLASS_BRIDGE_PCI) {
    LOG_INFO("[PCI]   Bridge: secondary=%u subordinate=%u", dev->secondary_bus,
             dev->subordinate_bus);
  }
}

static int pci_init_impl(void) {
  LOG_INFO("[PCI] Iniciando enumeracion de dispositivos...");

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
      }
    }
  }

  LOG_INFO("[PCI] Enumeracion completada (%d dispositivos)", count);
  return 0;
}

struct driver pci_driver = {
    .name = "pci",
    .init = pci_init_impl,
    .shutdown = NULL,
};