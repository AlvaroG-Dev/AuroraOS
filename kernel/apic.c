// kernel/apic.c
#include "apic.h"
#include "acpi.h"
#include "cpu.h"
#include "klog.h"
#include "paging.h"
#include "panic.h"

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------

// Dirección virtual del LAPIC (mapeada en MMIO_MAP_BASE).
static volatile uint32_t *g_lapic = NULL;

// APIC ID del BSP, leído al inicializar.
static uint32_t g_bsp_apic_id = 0;

// Tabla de ISOs del MADT: mapea IRQ legacy → GSI del IOAPIC.
// Por defecto, irq N → gsi N (identidad). Los ISOs pueden alterarlo.
// Ej: en muchos sistemas, IRQ 0 → GSI 2.
static uint8_t g_irq_to_gsi[16];

// Flag para saber si el LAPIC está activo.
static int g_apic_initialized = 0;

// ---------------------------------------------------------------------------
// Helpers de LAPIC
// ---------------------------------------------------------------------------
uint32_t lapic_read(uint32_t reg) {
  if (!g_lapic)
    return 0;
  return g_lapic[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value) {
  if (!g_lapic)
    return;
  g_lapic[reg / 4] = value;
}

void lapic_eoi(void) {
  if (!g_lapic)
    return;
  lapic_write(LAPIC_REG_EOI, 0);
}

uint32_t lapic_get_id(void) {
  if (!g_lapic)
    return 0;
  return lapic_read(LAPIC_REG_ID) >> 24;
}

uint32_t lapic_get_bsp_id(void) { return g_bsp_apic_id; }

// ---------------------------------------------------------------------------
// Helpers de IOAPIC
// ---------------------------------------------------------------------------
static uint32_t ioapic_read(uint32_t ioapic_addr, uint32_t reg) {
  volatile uint32_t *base = (volatile uint32_t *)ioapic_addr;
  base[IOAPIC_REG_SELECT / 4] = reg;
  return base[IOAPIC_REG_DATA / 4];
}

static void ioapic_write(uint32_t ioapic_addr, uint32_t reg, uint32_t value) {
  volatile uint32_t *base = (volatile uint32_t *)ioapic_addr;
  base[IOAPIC_REG_SELECT / 4] = reg;
  base[IOAPIC_REG_DATA / 4] = value;
}

// Escribe una entrada de redirección de 64 bits en el IOAPIC.
// `irq` es el índice de GSI (global system interrupt).
static void ioapic_write_redir(uint32_t ioapic_addr, uint32_t gsi,
                               uint64_t value) {
  uint32_t low_reg = 0x10 + gsi * 2;
  uint32_t high_reg = low_reg + 1;
  ioapic_write(ioapic_addr, low_reg, (uint32_t)(value & 0xFFFFFFFF));
  ioapic_write(ioapic_addr, high_reg, (uint32_t)(value >> 32));
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void apic_init(void) {
  const acpi_info_t *acpi = acpi_get_info();
  if (!acpi->valid) {
    LOG_ERR("[APIC] ACPI no válido, no se puede inicializar APIC");
    return;
  }

  // 1. Mapear el LAPIC en MMIO_MAP_BASE. La página del LAPIC ocupa
  //    4 KB aunque solo usemos los primeros 0x400 bytes.
  void *lapic_map = mmio_map(acpi->lapic_address, 0x1000,
                             PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!lapic_map) {
    LOG_ERR("[APIC] mmio_map del LAPIC (0x%lx) falló",
            (unsigned long)acpi->lapic_address);
    return;
  }
  g_lapic = (volatile uint32_t *)lapic_map;
  LOG_INFO("[APIC] LAPIC mapeado en 0x%lx (físico 0x%lx)",
           (unsigned long)lapic_map, (unsigned long)acpi->lapic_address);

  // 2. Activar el LAPIC vía MSR IA32_APIC_BASE (0x1B).
  uint64_t apic_base = rdmsr(0x1B);
  apic_base |= (1ULL << 11);  // Bit 11: APIC Global Enable
  apic_base &= ~(1ULL << 10); // Bit 10: BSP (no cambiar)
  wrmsr(0x1B, apic_base);

  // 3. Leer el APIC ID del LAPIC y comparar con el BSP detectado.
  uint32_t lapic_id = lapic_read(LAPIC_REG_ID) >> 24;
  g_bsp_apic_id = lapic_id;
  LOG_INFO("[APIC] LAPIC ID = %u", lapic_id);

  // El ID real del BSP lo habíamos detectado vía CPUID en acpi_init.
  // Si coinciden, todo bien. Si no, algo va mal.
  // (No lo comparamos aquí porque ya lo hicimos en acpi_init.)

  // 4. Configurar el SVR: habilitar el LAPIC + vector espurio 0xFF.
  // El vector espurio es el que se entrega cuando una interrupción
  // llega "en el momento equivocado". 0xFF es el valor recomendado.
  lapic_write(LAPIC_REG_SVR, 0xFF | LAPIC_SVR_ENABLE);

  // 5. Poner el TPR (Task Priority Register) a 0: aceptar todas las
  // interrupciones, sin prioridad mínima.
  lapic_write(LAPIC_REG_TPR, 0);

  // 6. Enmascarar los LVT por defecto, EXCEPTO LINT0.
  //
  // LINT0: la línea por la que el PIC 8259 entrega sus IRQs al LAPIC.
  //        Debe estar en modo ExtINT y DESENMASCARADO para que las
  //        IRQs del PIC sigan llegando al CPU mientras seguimos
  //        usando el PIC. Si la enmascaramos, el timer (IRQ 0) y el
  //        teclado (IRQ 1) dejan de llegar y el kernel se cuelga en
  //        el primer hlt.
  //
  //        Delivery mode ExtINT = 7 << 8.
  lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_THERMAL, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_PERF, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_LINT0, (7 << 8)); // ExtINT, sin máscara
  lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_ERROR, LAPIC_LVT_MASKED);

  // 7. Limpiar el ESR (Error Status Register) leyéndolo y escribiéndolo.
  lapic_write(LAPIC_REG_ESR, 0);
  (void)lapic_read(LAPIC_REG_ESR);

  // 8. Configurar la tabla de IRQ → GSI. Por defecto identidad.
  for (int i = 0; i < 16; i++)
    g_irq_to_gsi[i] = (uint8_t)i;

  // 9. Aplicar los ISOs del MADT si los hay. Re-parseamos el MADT
  //    para extraer los ISOs, o los pedimos a acpi. Aquí simplificamos:
  //    los ISOs ya se loguearon en acpi_init, pero no los guardamos.
  //    TODO: extender acpi_info_t para guardar los ISOs y aplicarlos aquí.

  g_apic_initialized = 1;
  LOG_INFO("[APIC] LAPIC activo y configurado");
}

// ---------------------------------------------------------------------------
// IOAPIC: redirecciones
// ---------------------------------------------------------------------------
void ioapic_redirect_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id,
                         int masked) {
  if (!g_apic_initialized)
    return;

  const acpi_info_t *acpi = acpi_get_info();
  if (acpi->ioapic_count == 0) {
    LOG_ERR("[APIC] No hay IOAPICs");
    return;
  }

  // Convertir IRQ legacy a GSI usando la tabla.
  uint8_t gsi = g_irq_to_gsi[irq & 0x0F];

  // Buscar el IOAPIC que cubre ese GSI.
  const acpi_ioapic_t *io = NULL;
  for (int i = 0; i < acpi->ioapic_count; i++) {
    const acpi_ioapic_t *cand = &acpi->ioapics[i];
    // Asumimos un solo IOAPIC o que el GSI cae dentro del rango
    // del IOAPIC. Rango: [gsi_base, gsi_base + 24) típicamente.
    if (gsi >= cand->gsi_base && gsi < cand->gsi_base + 24) {
      io = cand;
      break;
    }
  }
  if (!io) {
    LOG_ERR("[APIC] Ningún IOAPIC cubre GSI %u (irq %u)", gsi, irq);
    return;
  }

  // Mapear el IOAPIC en MMIO si no está ya mapeado.
  // (Lo haremos una vez por IOAPIC.)
  void *ioapic_map =
      mmio_map(io->address, 0x1000, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!ioapic_map) {
    LOG_ERR("[APIC] mmio_map del IOAPIC 0x%x falló", io->address);
    return;
  }

  // Construir la entrada de redirección de 64 bits:
  //   bits 0-7   : vector
  //   bits 8-10  : delivery mode (0 = fixed)
  //   bit  11    : destination mode (0 = physical)
  //   bit  12    : delivery status (ro)
  //   bit  13    : polarity (0 = active high, 1 = active low)
  //   bit  14    : remote irr (ro)
  //   bit  15    : trigger mode (0 = edge, 1 = level)
  //   bit  16    : masked
  //   bits 56-63 : destination APIC ID
  uint64_t entry = 0;
  entry |= (uint64_t)vector;
  entry |= ((uint64_t)dest_apic_id << 56);
  if (masked)
    entry |= IOAPIC_REDIR_MASKED;

  uint32_t gsi_in_ioapic = gsi - io->gsi_base;

  uint32_t ioapic_virt = (uint32_t)(uintptr_t)ioapic_map;
  ioapic_write_redir(ioapic_virt, gsi_in_ioapic, entry);

  LOG_INFO("[APIC] IRQ %u -> GSI %u -> vector %u en IOAPIC (dest APIC %u)%s",
           irq, gsi, vector, dest_apic_id, masked ? " [masked]" : "");
}

void ioapic_mask_irq(uint8_t irq, int masked) {
  if (!g_apic_initialized)
    return;

  const acpi_info_t *acpi = acpi_get_info();
  if (acpi->ioapic_count == 0)
    return;

  uint8_t gsi = g_irq_to_gsi[irq & 0x0F];

  const acpi_ioapic_t *io = NULL;
  for (int i = 0; i < acpi->ioapic_count; i++) {
    const acpi_ioapic_t *cand = &acpi->ioapics[i];
    if (gsi >= cand->gsi_base && gsi < cand->gsi_base + 24) {
      io = cand;
      break;
    }
  }
  if (!io)
    return;

  void *ioapic_map =
      mmio_map(io->address, 0x1000, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!ioapic_map)
    return;

  uint32_t gsi_in_ioapic = gsi - io->gsi_base;
  uint32_t ioapic_virt = (uint32_t)(uintptr_t)ioapic_map;

  uint32_t low_reg = 0x10 + gsi_in_ioapic * 2;
  uint32_t low = ioapic_read(ioapic_virt, low_reg);
  if (masked)
    low |= (uint32_t)IOAPIC_REDIR_MASKED;
  else
    low &= ~(uint32_t)IOAPIC_REDIR_MASKED;
  ioapic_write(ioapic_virt, low_reg, low);
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
void apic_dump(void) {
  if (!g_apic_initialized) {
    LOG_INFO("[APIC] No inicializado");
    return;
  }
  LOG_INFO("[APIC] Estado:");
  LOG_INFO("  BSP APIC ID = %u", g_bsp_apic_id);
  LOG_INFO("  LAPIC ID (actual) = %u", lapic_get_id());
  LOG_INFO("  LAPIC version = 0x%x", lapic_read(LAPIC_REG_VERSION) & 0xFF);
  LOG_INFO("  LAPIC SVR = 0x%x", lapic_read(LAPIC_REG_SVR));
  LOG_INFO("  LAPIC TPR = 0x%x", lapic_read(LAPIC_REG_TPR));
}