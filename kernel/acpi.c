// kernel/acpi.c
#include "acpi.h"
#include "cpu.h"
#include "klog.h"
#include "paging.h"
#include "panic.h"
#include "string.h"

// ---------------------------------------------------------------------------
// Estructuras ACPI (subset mínimo)
// ---------------------------------------------------------------------------

typedef struct __attribute__((packed)) {
  char signature[8];
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
  acpi_sdt_header_t header;
  uint32_t local_apic_address;
  uint32_t flags;
} madt_t;

typedef struct __attribute__((packed)) {
  uint8_t type;
  uint8_t length;
} madt_entry_header_t;

typedef struct __attribute__((packed)) {
  madt_entry_header_t header;
  uint8_t acpi_processor_id;
  uint8_t apic_id;
  uint32_t flags;
} madt_entry_lapic_t;

typedef struct __attribute__((packed)) {
  madt_entry_header_t header;
  uint8_t ioapic_id;
  uint8_t reserved;
  uint32_t ioapic_address;
  uint32_t gsi_base;
} madt_entry_ioapic_t;

typedef struct __attribute__((packed)) {
  madt_entry_header_t header;
  uint8_t bus;
  uint8_t source;
  uint32_t gsi;
  uint16_t flags;
} madt_entry_iso_t;

typedef struct __attribute__((packed)) {
  madt_entry_header_t header;
  uint16_t reserved;
  uint64_t lapic_address;
} madt_entry_lapic_override_t;

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
static acpi_info_t g_acpi = {0};

// Verifica el checksum de una tabla ACPI.
static int acpi_checksum_ok(const void *table, uint32_t length) {
  const uint8_t *p = (const uint8_t *)table;
  uint8_t sum = 0;
  for (uint32_t i = 0; i < length; i++)
    sum += p[i];
  return sum == 0;
}

// Busca una tabla con la firma dada en un array de punteros.
static acpi_sdt_header_t *find_table(uint64_t *root_entries, int count,
                                     int is_64bit, const char *signature) {
  for (int i = 0; i < count; i++) {
    uint64_t addr = is_64bit ? root_entries[i] : ((uint32_t *)root_entries)[i];
    if (addr == 0)
      continue;
    acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)phys_to_virt(addr);
    if (memcmp(hdr->signature, signature, 4) == 0) {
      return hdr;
    }
  }
  return NULL;
}

// Parsea el MADT y rellena g_acpi.
static void parse_madt(const madt_t *madt) {
  g_acpi.lapic_address = madt->local_apic_address;

  LOG_INFO("[ACPI] LAPIC address (MADT) = 0x%lx",
           (unsigned long)g_acpi.lapic_address);

  if (madt->flags & 1) {
    LOG_INFO("[ACPI] MADT indica presencia de PIC dual 8259 legacy");
  }

  const uint8_t *p = (const uint8_t *)madt + sizeof(madt_t);
  const uint8_t *end = (const uint8_t *)madt + madt->header.length;

  while (p + sizeof(madt_entry_header_t) <= end) {
    const madt_entry_header_t *eh = (const madt_entry_header_t *)p;
    if (eh->length == 0)
      break;

    switch (eh->type) {
    case 0: {
      const madt_entry_lapic_t *lapic = (const madt_entry_lapic_t *)p;
      if (g_acpi.cpu_count < ACPI_MAX_CPUS) {
        acpi_cpu_t *cpu = &g_acpi.cpus[g_acpi.cpu_count];
        cpu->apic_id = lapic->apic_id;
        cpu->processor_id = lapic->acpi_processor_id;
        cpu->enabled = (lapic->flags & 1) ? 1 : 0;
        cpu->is_bsp = 0;
        LOG_INFO("[ACPI] CPU[%d]: apic_id=%u proc_id=%u %s", g_acpi.cpu_count,
                 cpu->apic_id, cpu->processor_id,
                 cpu->enabled ? "enabled" : "disabled");
        g_acpi.cpu_count++;
      } else {
        LOG_WARN("[ACPI] Demasiadas CPUs (ignorando apic_id=%u)",
                 lapic->apic_id);
      }
      break;
    }
    case 1: {
      const madt_entry_ioapic_t *io = (const madt_entry_ioapic_t *)p;
      if (g_acpi.ioapic_count < ACPI_MAX_IOAPICS) {
        acpi_ioapic_t *i = &g_acpi.ioapics[g_acpi.ioapic_count];
        i->id = io->ioapic_id;
        i->address = io->ioapic_address;
        i->gsi_base = io->gsi_base;
        LOG_INFO("[ACPI] IOAPIC[%d]: id=%u addr=0x%x gsi_base=%u",
                 g_acpi.ioapic_count, i->id, i->address, i->gsi_base);
        g_acpi.ioapic_count++;
      }
      break;
    }
    case 2: {
      const madt_entry_iso_t *iso = (const madt_entry_iso_t *)p;
      if (g_acpi.iso_count < ACPI_MAX_ISOS) {
        acpi_iso_t *s = &g_acpi.isos[g_acpi.iso_count++];
        s->irq = iso->source;
        s->gsi = iso->gsi;
        s->flags = iso->flags;
      }
      LOG_INFO("[ACPI] ISO: bus=%u irq=%u -> gsi=%u flags=0x%x", iso->bus,
               iso->source, iso->gsi, iso->flags);
      break;
    }
    case 5: {
      const madt_entry_lapic_override_t *lo =
          (const madt_entry_lapic_override_t *)p;
      g_acpi.lapic_address = lo->lapic_address;
      LOG_INFO("[ACPI] LAPIC address override = 0x%lx",
               (unsigned long)g_acpi.lapic_address);
      break;
    }
    default:
      break;
    }

    p += eh->length;
  }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void acpi_init(const uint8_t rsdp_bytes[64]) {
  LOG_INFO("[ACPI] Inicializando");

  if (memcmp(rsdp_bytes, "RSD PTR ", 8) != 0) {
    LOG_ERR("[ACPI] RSDP: firma inválida");
    g_acpi.valid = 0;
    return;
  }

  const rsdp_t *rsdp = (const rsdp_t *)rsdp_bytes;

  // Imprimir el OEM ID como string (5 chars + terminador).
  char oem_buf[7] = {0};
  for (int i = 0; i < 6; i++)
    oem_buf[i] = rsdp->oem_id[i];
  LOG_INFO("[ACPI] RSDP revision=%u oem=%s", rsdp->revision, oem_buf);

  acpi_sdt_header_t *root = NULL;
  int is_64bit = 0;
  uint64_t root_phys = 0;

  if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
    root = (acpi_sdt_header_t *)phys_to_virt(rsdp->xsdt_address);
    root_phys = rsdp->xsdt_address;
    is_64bit = 1;
    if (memcmp(root->signature, "XSDT", 4) != 0) {
      LOG_WARN("[ACPI] XSDT firma inválida, probando RSDT");
      root = NULL;
    }
  }

  if (!root && rsdp->rsdt_address != 0) {
    root = (acpi_sdt_header_t *)phys_to_virt(rsdp->rsdt_address);
    root_phys = rsdp->rsdt_address;
    is_64bit = 0;
    if (memcmp(root->signature, "RSDT", 4) != 0) {
      LOG_ERR("[ACPI] RSDT firma inválida");
      g_acpi.valid = 0;
      return;
    }
  }

  if (!root) {
    LOG_ERR("[ACPI] No se encontró RSDT ni XSDT");
    g_acpi.valid = 0;
    return;
  }

  LOG_INFO("[ACPI] %s en 0x%lx (length=%u)", is_64bit ? "XSDT" : "RSDT",
           (unsigned long)root_phys, root->length);

  if (!acpi_checksum_ok(root, root->length)) {
    LOG_WARN("[ACPI] Checksum del %s inválido (continuamos igualmente)",
             is_64bit ? "XSDT" : "RSDT");
  }

  int entries_count =
      (root->length - sizeof(acpi_sdt_header_t)) / (is_64bit ? 8 : 4);
  LOG_DEBUG("[ACPI] %d entradas en %s", entries_count,
            is_64bit ? "XSDT" : "RSDT");

  uint64_t *entries = (uint64_t *)((uint8_t *)root + sizeof(acpi_sdt_header_t));
  acpi_sdt_header_t *madt_hdr =
      find_table(entries, entries_count, is_64bit, "APIC");
  if (!madt_hdr) {
    LOG_ERR("[ACPI] MADT (APIC) no encontrado");
    g_acpi.valid = 0;
    return;
  }

  LOG_INFO("[ACPI] MADT en 0x%lx (length=%u)",
           (unsigned long)((uint64_t)madt_hdr - PHYS_MAP_BASE),
           madt_hdr->length);

  if (!acpi_checksum_ok(madt_hdr, madt_hdr->length)) {
    LOG_WARN("[ACPI] Checksum del MADT inválido (continuamos igualmente)");
  }

  parse_madt((const madt_t *)madt_hdr);

  uint32_t eax, ebx, ecx, edx;
  cpuid(1, 0, &eax, &ebx, &ecx, &edx);
  uint32_t bsp_apic_id = (ebx >> 24) & 0xFF;
  LOG_INFO("[ACPI] BSP APIC ID = %u (via CPUID)", bsp_apic_id);

  g_acpi.bsp_index = -1;
  for (int i = 0; i < g_acpi.cpu_count; i++) {
    if (g_acpi.cpus[i].apic_id == bsp_apic_id) {
      g_acpi.cpus[i].is_bsp = 1;
      g_acpi.bsp_index = i;
      break;
    }
  }

  if (g_acpi.bsp_index < 0) {
    LOG_WARN("[ACPI] BSP no encontrado en la lista de CPUs");
  }

  g_acpi.valid = 1;
  LOG_INFO("[ACPI] Parseo completo: %d CPUs, %d IOAPICs, %d ISOs",
           g_acpi.cpu_count, g_acpi.ioapic_count, g_acpi.iso_count);
}

const acpi_info_t *acpi_get_info(void) { return &g_acpi; }

void acpi_dump(void) {
  if (!g_acpi.valid) {
    LOG_INFO("[ACPI] No inicializado o inválido");
    return;
  }
  LOG_INFO("[ACPI] Estado:");
  LOG_INFO("  LAPIC address: 0x%lx", (unsigned long)g_acpi.lapic_address);
  LOG_INFO("  BSP index: %d", g_acpi.bsp_index);
  LOG_INFO("  CPUs (%d):", g_acpi.cpu_count);
  for (int i = 0; i < g_acpi.cpu_count; i++) {
    const acpi_cpu_t *c = &g_acpi.cpus[i];
    LOG_INFO("    [%d] apic_id=%u proc_id=%u enabled=%d %s", i, c->apic_id,
             c->processor_id, c->enabled, c->is_bsp ? "BSP" : "");
  }
  LOG_INFO("  IOAPICs (%d):", g_acpi.ioapic_count);
  for (int i = 0; i < g_acpi.ioapic_count; i++) {
    const acpi_ioapic_t *io = &g_acpi.ioapics[i];
    LOG_INFO("    [%d] id=%u addr=0x%x gsi_base=%u", i, io->id, io->address,
             io->gsi_base);
  }
  LOG_INFO("  ISOs (%d):", g_acpi.iso_count);
  for (int i = 0; i < g_acpi.iso_count; i++) {
    const acpi_iso_t *s = &g_acpi.isos[i];
    LOG_INFO("    [%d] irq=%u -> gsi=%u flags=0x%x", i, s->irq, s->gsi,
             s->flags);
  }
}