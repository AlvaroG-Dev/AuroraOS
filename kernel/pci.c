// kernel/pci.c
#include "pci.h"
#include "io.h"
#include "klog.h"
#include "serial.h"

static const char *pci_class_name(uint8_t class_id) {
  switch (class_id) {
  case PCI_CLASS_UNCLASSIFIED:
    return "Unclassified";
  case PCI_CLASS_STORAGE:
    return "Mass Storage Controller";
  case PCI_CLASS_NETWORK:
    return "Network Controller";
  case PCI_CLASS_DISPLAY:
    return "Display Controller";
  case PCI_CLASS_MULTIMEDIA:
    return "Multimedia Controller";
  case PCI_CLASS_MEMORY:
    return "Memory Controller";
  case PCI_CLASS_BRIDGE:
    return "Bridge";
  case PCI_CLASS_COMMUNICATION:
    return "Simple Communication Controller";
  case PCI_CLASS_GENERIC_SYSTEM:
    return "Base System Peripheral";
  case PCI_CLASS_INPUT:
    return "Input Device Controller";
  case PCI_CLASS_DOCKING:
    return "Docking Station";
  case PCI_CLASS_PROCESSOR:
    return "Processor";
  case PCI_CLASS_SERIAL_BUS:
    return "Serial Bus Controller";
  case PCI_CLASS_WIRELESS:
    return "Wireless Controller";
  case PCI_CLASS_INTELLIGENT_IO:
    return "Intelligent Controller";
  case PCI_CLASS_SATELLITE:
    return "Satellite Communication Controller";
  case PCI_CLASS_ENCRYPTION:
    return "Encryption Controller";
  case PCI_CLASS_SIGNAL_PROCESSING:
    return "Signal Processing Controller";
  case PCI_CLASS_PROCESSING_ACCEL:
    return "Processing Accelerator";
  case PCI_CLASS_NON_ESSENTIAL:
    return "Non-Essential Instrumentation";
  default:
    return "Unknown";
  }
}

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
  return (uint16_t)((inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset, uint32_t value) {
  uint32_t address = PCI_MAKE_ADDRESS(bus, slot, func, offset);
  outl(PCI_CONFIG_ADDRESS, address);
  outl(PCI_CONFIG_DATA, value);
}

static void pci_check_device(uint8_t bus, uint8_t slot) {
  uint16_t vendor_id = pci_read_config_word(bus, slot, 0, 0);

  if (vendor_id == 0xFFFF)
    return;

  uint8_t header_type = (pci_read_config_word(bus, slot, 0, 0x0E) >> 8) & 0xFF;
  int num_funcs = (header_type & 0x80) ? 8 : 1;

  for (uint8_t func = 0; func < num_funcs; func++) {
    uint16_t dev_vendor_id = pci_read_config_word(bus, slot, func, 0);
    if (dev_vendor_id == 0xFFFF)
      continue;

    uint16_t device_id = pci_read_config_word(bus, slot, func, 2);
    uint16_t class_reg = pci_read_config_word(bus, slot, func, 0x0A);

    uint8_t class_id = (class_reg >> 8) & 0xFF;
    uint8_t subclass_id = class_reg & 0xFF;

    LOG_INFO("[PCI] Bus %d, Slot %d, Func %d | Vendor: 0x%X, Device: 0x%X | %s "
             "(Class 0x%X, Subclass 0x%X)",
             bus, slot, func, dev_vendor_id, device_id,
             pci_class_name(class_id), class_id, subclass_id);
  }
}

static int pci_init_impl(void) {
  LOG_INFO("[PCI] Iniciando enumeracion de dispositivos...");
  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      pci_check_device((uint8_t)bus, slot);
    }
  }
  LOG_INFO("[PCI] Enumeracion completada.");
  return 0;
}

struct driver pci_driver = {
    .name = "pci",
    .init = pci_init_impl,
    .shutdown = NULL,
};