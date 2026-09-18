// kernel/acpi.h
#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stddef.h>
#include <stdint.h>

#define ACPI_MAX_CPUS 64
#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_ISOS 16

typedef struct {
  uint32_t apic_id;
  uint32_t processor_id;
  int enabled;
  int is_bsp;
} acpi_cpu_t;

typedef struct {
  uint8_t id;
  uint32_t address;
  uint32_t gsi_base;
} acpi_ioapic_t;

typedef struct {
  uint8_t irq;
  uint32_t gsi;
  uint16_t flags;
} acpi_iso_t;

// ---------------------------------------------------------------------------
// [SMP] Multiprocessor Wakeup Structure (ACPI 6.4, sección 5.2.12.19)
//
// Si el firmware la expone en el MADT (type 0x10), podemos arrancar APs
// escribiendo en un mailbox en lugar de usar INIT-SIPI-SIPI. Es la forma
// moderna y funciona en QEMU+OVMF donde INIT-SIPI-SIPI no arranca los APs.
// ---------------------------------------------------------------------------
typedef struct {
  int present;            // 1 si el firmware la expone
  uint16_t version;       // versión de la estructura (1 o 2)
  uint64_t mailbox_paddr; // dirección física del mailbox
} acpi_wakeup_t;

// Layout del mailbox (los primeros 16 bytes son comunes a v1 y v2).
// Ref: ACPI 6.4, sección 5.2.12.19.
typedef struct __attribute__((packed)) {
  uint16_t command; // 0 = idle, 1 = wakeup
  uint16_t reserved;
  uint32_t apic_id;       // APIC ID del AP a despertar
  uint64_t wakeup_vector; // dirección virtual del entry point del AP
} acpi_wakeup_mailbox_t;

#define ACPI_MP_WAKE_COMMAND_IDLE 0
#define ACPI_MP_WAKE_COMMAND_WAKEUP 1

typedef struct {
  int valid;
  uint64_t lapic_address;
  int cpu_count;
  acpi_cpu_t cpus[ACPI_MAX_CPUS];
  int ioapic_count;
  acpi_ioapic_t ioapics[ACPI_MAX_IOAPICS];
  int bsp_index;

  int iso_count;
  acpi_iso_t isos[ACPI_MAX_ISOS];

  // [SMP] Multiprocessor Wakeup Structure.
  acpi_wakeup_t wakeup;
} acpi_info_t;

void acpi_init(const uint8_t rsdp_bytes[64]);
const acpi_info_t *acpi_get_info(void);
void acpi_dump(void);

#endif