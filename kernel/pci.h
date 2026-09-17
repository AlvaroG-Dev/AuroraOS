// kernel/pci.h
#ifndef PCI_H
#define PCI_H

#include "driver.h"
#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_MAKE_ADDRESS(bus, slot, func, offset)                              \
  ((uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) |     \
              ((uint32_t)0x80000000)))

// PCI Device Class Codes
#define PCI_CLASS_UNCLASSIFIED 0x00
#define PCI_CLASS_STORAGE 0x01
#define PCI_CLASS_NETWORK 0x02
#define PCI_CLASS_DISPLAY 0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_MEMORY 0x05
#define PCI_CLASS_BRIDGE 0x06
#define PCI_CLASS_COMMUNICATION 0x07
#define PCI_CLASS_GENERIC_SYSTEM 0x08
#define PCI_CLASS_INPUT 0x09
#define PCI_CLASS_DOCKING 0x0A
#define PCI_CLASS_PROCESSOR 0x0B
#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_CLASS_WIRELESS 0x0D
#define PCI_CLASS_INTELLIGENT_IO 0x0E
#define PCI_CLASS_SATELLITE 0x0F
#define PCI_CLASS_ENCRYPTION 0x10
#define PCI_CLASS_SIGNAL_PROCESSING 0x11
#define PCI_CLASS_PROCESSING_ACCEL 0x12
#define PCI_CLASS_NON_ESSENTIAL 0x13
#define PCI_CLASS_CO_PROCESSOR 0x40
#define PCI_CLASS_UNASSIGNED 0xFF

// Subclass
#define PCI_SUBCLASS_SATA 0x06
#define PCI_SUBCLASS_IDE 0x01
#define PCI_SUBCLASS_VGA 0x00
#define PCI_SUBCLASS_ETHERNET 0x00

typedef struct {
  uint8_t bus;
  uint8_t slot;
  uint8_t func;
  uint16_t vendor_id;
  uint16_t device_id;
  uint8_t class_id;
  uint8_t subclass_id;
  uint8_t prog_if;
  uint8_t revision_id;
  uint8_t header_type;
} pci_device_t;

extern struct driver pci_driver;

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t offset);
uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t offset);
void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset, uint32_t value);

#endif