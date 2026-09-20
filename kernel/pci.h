// kernel/pci.h
#ifndef PCI_H
#define PCI_H

#include "driver.h"
#include <stdint.h>

// ===========================================================================
// Puertos de configuración PCI.
// ===========================================================================
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_MAKE_ADDRESS(bus, slot, func, offset)                              \
  ((uint32_t)(((uint32_t)(bus) << 16) | ((uint32_t)(slot) << 11) |             \
              ((uint32_t)(func) << 8) | ((uint32_t)(offset) & 0xFC) |          \
              0x80000000U))

// ===========================================================================
// Clases de dispositivo PCI.
// ===========================================================================
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

// ===========================================================================
// Subclases relevantes.
// ===========================================================================
#define PCI_SUBCLASS_STORAGE_SCSI 0x00
#define PCI_SUBCLASS_STORAGE_IDE 0x01
#define PCI_SUBCLASS_STORAGE_FLOPPY 0x02
#define PCI_SUBCLASS_STORAGE_IPI 0x03
#define PCI_SUBCLASS_STORAGE_RAID 0x04
#define PCI_SUBCLASS_STORAGE_ATA 0x05
#define PCI_SUBCLASS_STORAGE_SATA 0x06
#define PCI_SUBCLASS_STORAGE_SAS 0x07
#define PCI_SUBCLASS_STORAGE_NVME 0x08
#define PCI_SUBCLASS_STORAGE_OTHER 0x80

#define PCI_SUBCLASS_BRIDGE_HOST 0x00
#define PCI_SUBCLASS_BRIDGE_ISA 0x01
#define PCI_SUBCLASS_BRIDGE_EISA 0x02
#define PCI_SUBCLASS_BRIDGE_MCA 0x03
#define PCI_SUBCLASS_BRIDGE_PCI 0x04
#define PCI_SUBCLASS_BRIDGE_PCMCIA 0x05
#define PCI_SUBCLASS_BRIDGE_NUBUS 0x06
#define PCI_SUBCLASS_BRIDGE_CARDBUS 0x07
#define PCI_SUBCLASS_BRIDGE_OTHER 0x80

#define PCI_SUBCLASS_DISPLAY_VGA 0x00
#define PCI_SUBCLASS_DISPLAY_XGA 0x01
#define PCI_SUBCLASS_DISPLAY_3D 0x02
#define PCI_SUBCLASS_DISPLAY_OTHER 0x80

#define PCI_SUBCLASS_NETWORK_ETHERNET 0x00
#define PCI_SUBCLASS_NETWORK_TOKENRING 0x01
#define PCI_SUBCLASS_NETWORK_FDDI 0x02
#define PCI_SUBCLASS_NETWORK_ATM 0x03
#define PCI_SUBCLASS_NETWORK_OTHER 0x80

// ===========================================================================
// Prog IF (offset 0x09) para IDE.
// ===========================================================================
#define PCI_PROGIF_IDE_COMPAT 0x00
#define PCI_PROGIF_IDE_NATIVE 0x05
#define PCI_PROGIF_IDE_BUS_MASTER 0x80
#define PCI_PROGIF_AHCI 0x80

// ===========================================================================
// Tipos de header PCI.
// ===========================================================================
#define PCI_HEADER_TYPE_DEVICE 0x00
#define PCI_HEADER_TYPE_BRIDGE 0x01
#define PCI_HEADER_TYPE_CARDBUS 0x02

// ===========================================================================
// Bits de PCI Command (offset 0x04).
// ===========================================================================
#define PCI_CMD_IO_SPACE (1 << 0)
#define PCI_CMD_MEM_SPACE (1 << 1)
#define PCI_CMD_BUS_MASTER (1 << 2)
#define PCI_CMD_SPECIAL_CYCLES (1 << 3)
#define PCI_CMD_MEM_WRITE_INV (1 << 4)
#define PCI_CMD_VGA_PALETTE (1 << 5)
#define PCI_CMD_PARITY_ERR (1 << 6)
#define PCI_CMD_SERR_ENABLE (1 << 8)
#define PCI_CMD_FAST_BACK (1 << 9)
#define PCI_CMD_INT_DISABLE (1 << 10)

// ===========================================================================
// Bits de PCI Status (offset 0x06).
// ===========================================================================
#define PCI_STATUS_INT_STATUS (1 << 3)
#define PCI_STATUS_CAP_LIST (1 << 4)
#define PCI_STATUS_66MHZ (1 << 5)
#define PCI_STATUS_FAST_BACK (1 << 7)
#define PCI_STATUS_PARITY_ERR (1 << 8)
#define PCI_STATUS_DEVSEL_MASK (0x3 << 9)
#define PCI_STATUS_SIG_TARGET_ABORT (1 << 11)
#define PCI_STATUS_SIG_MASTER_ABORT (1 << 13)

// ===========================================================================
// Estructura que describe un dispositivo PCI.
// ===========================================================================
typedef struct {
  uint8_t bus;
  uint8_t slot;
  uint8_t func;

  uint16_t vendor_id;
  uint16_t device_id;
  uint16_t subsystem_vendor_id;
  uint16_t subsystem_id;

  uint8_t class_id;
  uint8_t subclass_id;
  uint8_t prog_if;
  uint8_t revision_id;

  uint8_t header_type; // bits 0-6
  uint8_t multifunction;
  uint8_t irq_line;
  uint8_t irq_pin;

  // BARs.
  uint32_t bar[6];
  uint8_t bar_is_io[6]; // 1 si el BAR es I/O, 0 si es MMIO
  uint8_t bar_is_64[6]; // 1 si es un BAR de 64 bits (ocupa 2 slots)

  // Para bridges.
  uint8_t secondary_bus;
  uint8_t subordinate_bus;
} pci_device_t;

// ===========================================================================
// API de configuración de bajo nivel.
// ===========================================================================
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t offset);
uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t offset);
uint8_t pci_read_config_byte(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t offset);

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset, uint32_t value);
void pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset, uint16_t value);
void pci_write_config_byte(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset, uint8_t value);

// ===========================================================================
// API de alto nivel.
// ===========================================================================

// Rellena `dev` con la info del dispositivo (bus, slot, func).
// Devuelve 0 si OK, -1 si no existe.
int pci_get_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t *dev);

// Busca el primer dispositivo que coincida con class_id y subclass_id.
// Si subclass_id es 0xFF, cualquier subclase vale.
// Devuelve 0 si lo encuentra, -1 si no.
int pci_find_device(uint8_t class_id, uint8_t subclass_id, pci_device_t *dev);

// Busca el siguiente dispositivo (para iterar).
// `start_bus`, `start_slot`, `start_func` marcan el punto de inicio.
// `found_bus`, `found_slot`, `found_func` devuelven el encontrado.
// Devuelve 0 si OK, -1 si no hay más.
int pci_find_next_device(uint8_t class_id, uint8_t subclass_id,
                         uint8_t *start_bus, uint8_t *start_slot,
                         uint8_t *start_func, pci_device_t *dev);

// Devuelve el BAR `bar_num` del dispositivo. 0 si no existe.
uint32_t pci_get_bar(const pci_device_t *dev, int bar_num);

// Lee el BAR `bar_num` del dispositivo.
uint32_t pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func, int bar_num);

// Habilita bus mastering (necesario para DMA).
void pci_enable_bus_mastering(const pci_device_t *dev);

// Habilita el espacio de I/O y memoria del dispositivo.
void pci_enable_io_mem(const pci_device_t *dev);

// Devuelve el IRQ line del dispositivo (0xFF si no tiene).
uint8_t pci_get_irq(const pci_device_t *dev);

// ===========================================================================
// Driver.
// ===========================================================================
extern struct driver pci_driver;

#endif