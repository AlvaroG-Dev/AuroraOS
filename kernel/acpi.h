// kernel/acpi.h
#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stddef.h>
#include <stdint.h>

// Número máximo de CPUs y IOAPICs que soportamos.
#define ACPI_MAX_CPUS 64
#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_ISOS 16

// Información de una CPU descubierta en el MADT.
typedef struct {
  uint32_t apic_id;
  uint32_t processor_id;
  int enabled;
  int is_bsp;
} acpi_cpu_t;

// Información de un IOAPIC.
typedef struct {
  uint8_t id;
  uint32_t address;
  uint32_t gsi_base;
} acpi_ioapic_t;

// Interrupt Source Override (ISO) del MADT: mapea una IRQ legacy a un
// GSI del IOAPIC, con flags de polaridad/trigger.
typedef struct {
  uint8_t irq;    // IRQ legacy (0-15)
  uint32_t gsi;   // GSI en el IOAPIC
  uint16_t flags; // ISO flags (polaridad, trigger)
} acpi_iso_t;

// Información global de ACPI.
typedef struct {
  int valid;
  uint64_t lapic_address;
  int cpu_count;
  acpi_cpu_t cpus[ACPI_MAX_CPUS];
  int ioapic_count;
  acpi_ioapic_t ioapics[ACPI_MAX_IOAPICS];
  int bsp_index;

  // ISOs del MADT.
  int iso_count;
  acpi_iso_t isos[ACPI_MAX_ISOS];
} acpi_info_t;

// Inicializa ACPI. `rsdp_bytes` son los 64 bytes del RSDP copiados
// por el bootloader.
void acpi_init(const uint8_t rsdp_bytes[64]);

// Devuelve la info ACPI global (solo válida tras acpi_init).
const acpi_info_t *acpi_get_info(void);

// Debug.
void acpi_dump(void);

#endif