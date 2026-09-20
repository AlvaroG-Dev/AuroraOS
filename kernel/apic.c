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

// Punteros virtuales a los IOAPICs mapeados.
static volatile uint32_t *g_ioapics[ACPI_MAX_IOAPICS];

// APIC ID del BSP, leído al inicializar.
static uint32_t g_bsp_apic_id = 0;

// Tabla de IRQ legacy → GSI del IOAPIC. Por defecto identidad. Los ISOs
// del MADT pueden modificarla (por ejemplo, IRQ 0 → GSI 2).
// Un valor 0xFF indica que la IRQ NO es ruteable (colisión de GSI).
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
static uint32_t ioapic_read_reg(int ioapic_idx, uint32_t reg) {
  if (ioapic_idx < 0 || ioapic_idx >= ACPI_MAX_IOAPICS)
    return 0;
  volatile uint32_t *base = g_ioapics[ioapic_idx];
  if (!base)
    return 0;
  base[IOAPIC_REG_SELECT / 4] = reg;
  return base[IOAPIC_REG_DATA / 4];
}

static void ioapic_write_reg(int ioapic_idx, uint32_t reg, uint32_t value) {
  if (ioapic_idx < 0 || ioapic_idx >= ACPI_MAX_IOAPICS)
    return;
  volatile uint32_t *base = g_ioapics[ioapic_idx];
  if (!base)
    return;
  base[IOAPIC_REG_SELECT / 4] = reg;
  base[IOAPIC_REG_DATA / 4] = value;
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

  // 1. Mapear el LAPIC en MMIO_MAP_BASE.
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

  // 2. Mapear todos los IOAPICs.
  for (int i = 0; i < acpi->ioapic_count; i++) {
    const acpi_ioapic_t *io = &acpi->ioapics[i];
    void *map =
        mmio_map(io->address, 0x1000, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
    if (!map) {
      LOG_ERR("[APIC] No se pudo mapear IOAPIC 0x%x", io->address);
      g_ioapics[i] = NULL;
      continue;
    }
    g_ioapics[i] = (volatile uint32_t *)map;
    LOG_INFO("[APIC] IOAPIC[%d] mapeado en 0x%lx (físico 0x%x)", i,
             (unsigned long)map, io->address);
  }

  // 3. Activar el LAPIC vía MSR IA32_APIC_BASE (0x1B).
  uint64_t apic_base = rdmsr(0x1B);
  apic_base |= (1ULL << 11); // Bit 11: APIC Global Enable
  wrmsr(0x1B, apic_base);

  // 4. Leer el APIC ID del LAPIC.
  uint32_t lapic_id = lapic_read(LAPIC_REG_ID) >> 24;
  g_bsp_apic_id = lapic_id;
  LOG_INFO("[APIC] LAPIC ID = %u", lapic_id);

  // 5. Configurar el SVR: habilitar LAPIC + vector espurio 0xFF.
  lapic_write(LAPIC_REG_SVR, 0xFF | LAPIC_SVR_ENABLE);

  // 6. Poner el TPR a 0.
  lapic_write(LAPIC_REG_TPR, 0);

  // 7. Configurar los LVT.
  //
  // En sub-fase 2.2 ya NO usamos el PIC. Las IRQs llegan por el IOAPIC
  // directamente al LAPIC (por el bus APIC interno). El LINT0, que era
  // el canal del PIC, se enmascara. Si lo dejáramos en ExtINT, el PIC
  // (aunque enmascarado a nivel de hardware) podría generar interrupciones
  // espurias por el estado del pin, colgando el kernel.
  lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_THERMAL, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_PERF, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_ERROR, LAPIC_LVT_MASKED);

  // 8. Limpiar el ESR.
  lapic_write(LAPIC_REG_ESR, 0);
  (void)lapic_read(LAPIC_REG_ESR);

  // 9. Configurar la tabla IRQ → GSI. Por defecto identidad.
  for (int i = 0; i < 16; i++)
    g_irq_to_gsi[i] = (uint8_t)i;

  // 10. Aplicar los ISOs del MADT.
  for (int i = 0; i < acpi->iso_count; i++) {
    const acpi_iso_t *iso = &acpi->isos[i];
    if (iso->irq < 16) {
      g_irq_to_gsi[iso->irq] = (uint8_t)iso->gsi;
      LOG_INFO("[APIC] ISO aplicado: IRQ %u -> GSI %u", iso->irq, iso->gsi);
    }
  }

  // 10b. Detectar colisiones de GSI.
  //
  // Dos IRQs legacy distintas NO pueden apuntar al mismo GSI del IOAPIC:
  // si lo hacen, la segunda redirección sobrescribe la primera y solo
  // una de las dos IRQs funcionará. En sistemas con PIC legacy, el ISO
  // dice "IRQ 0 → GSI 2", pero por defecto "IRQ 2 → GSI 2" también se
  // cumple (identidad). Esto es una colisión. La IRQ 2 del PIC es la
  // cascada al PIC slave y NUNCA se usa como IRQ normal en sistemas
  // con PIC dual. La marcamos como no ruteable.
  {
    uint8_t gsi_owner[24];
    for (int i = 0; i < 24; i++)
      gsi_owner[i] = 0xFF;

    for (int irq = 0; irq < 16; irq++) {
      uint8_t gsi = g_irq_to_gsi[irq];
      if (gsi >= 24)
        continue;
      if (gsi_owner[gsi] == 0xFF) {
        gsi_owner[gsi] = (uint8_t)irq;
      } else {
        LOG_WARN("[APIC] IRQ %u colisiona con IRQ %u en GSI %u "
                 "(marcada como no ruteable)",
                 irq, gsi_owner[gsi], gsi);
        g_irq_to_gsi[irq] = 0xFF;
      }
    }
  }

  g_apic_initialized = 1;

  // 11. Redirigir todas las IRQs del PIC a sus vectores correspondientes
  //     en el IOAPIC, enmascaradas por defecto. Los drivers que instalen
  //     handlers las desenmascararán.
  for (int irq = 0; irq < 16; irq++) {
    if (g_irq_to_gsi[irq] == 0xFF)
      continue; // IRQ no ruteable (colisión)
    ioapic_redirect_irq(irq, 32 + irq, g_bsp_apic_id, 1); // masked
  }

  // 12. Enmascarar TODAS las IRQs del PIC para que no interfieran.
  __asm__ volatile("outb %0, $0x21" : : "a"((uint8_t)0xFF));
  __asm__ volatile("outb %0, $0xA1" : : "a"((uint8_t)0xFF));
  LOG_INFO("[APIC] PIC enmascarado completamente");

  LOG_INFO("[APIC] LAPIC activo y configurado");
}

// ---------------------------------------------------------------------------
// IOAPIC: redirecciones
//
// NOTA: `irq` se refiere a una IRQ legacy del PIC (0-15). Para IRQs de
// PCI (16+) habría que usar directamente el GSI del dispositivo, no
// pasar por g_irq_to_gsi[]. Eso es trabajo futuro.
// ---------------------------------------------------------------------------
void ioapic_redirect_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id,
                         int masked) {
  if (irq >= 16) {
    LOG_ERR("[APIC] ioapic_redirect_irq: IRQ %u fuera de rango (0-15)", irq);
    return;
  }

  const acpi_info_t *acpi = acpi_get_info();
  if (acpi->ioapic_count == 0) {
    LOG_ERR("[APIC] No hay IOAPICs");
    return;
  }

  uint8_t gsi = g_irq_to_gsi[irq];
  if (gsi == 0xFF) {
    LOG_DEBUG("[APIC] IRQ %u sin GSI, redirección ignorada", irq);
    return;
  }

  // Buscar el IOAPIC que cubre ese GSI.
  int ioapic_idx = -1;
  for (int i = 0; i < acpi->ioapic_count; i++) {
    const acpi_ioapic_t *cand = &acpi->ioapics[i];
    if (gsi >= cand->gsi_base && gsi < cand->gsi_base + 24) {
      ioapic_idx = i;
      break;
    }
  }
  if (ioapic_idx < 0 || !g_ioapics[ioapic_idx]) {
    LOG_ERR("[APIC] IOAPIC no encontrado para GSI %u (irq %u)", gsi, irq);
    return;
  }

  // Construir la entrada de redirección de 64 bits.
  uint64_t entry = 0;
  entry |= (uint64_t)vector;
  entry |= ((uint64_t)dest_apic_id << 56);

  // Aplicar flags del ISO si lo hay. Por defecto: activo alto, edge.
  int is_level = 0;
  for (int i = 0; i < acpi->iso_count; i++) {
    const acpi_iso_t *s = &acpi->isos[i];
    if (s->irq == irq) {
      if (s->flags & 0x2)
        entry |= IOAPIC_REDIR_ACTIVE_LOW;
      if (s->flags & 0x8)
        is_level = 1;
      break;
    }
  }

  // IRQs legacy del IDE (14, 15): el BMIDE genera IRQs level-triggered.
  // Si el IOAPIC está configurado para edge, la IRQ puede perderse.
  if (irq == 14 || irq == 15)
    is_level = 1;

  if (is_level)
    entry |= IOAPIC_REDIR_TRIGGER_LEVEL;

  if (masked)
    entry |= IOAPIC_REDIR_MASKED;

  uint32_t gsi_in_ioapic = gsi - acpi->ioapics[ioapic_idx].gsi_base;
  uint32_t low_reg = 0x10 + gsi_in_ioapic * 2;
  uint32_t high_reg = low_reg + 1;

  ioapic_write_reg(ioapic_idx, low_reg, (uint32_t)(entry & 0xFFFFFFFF));
  ioapic_write_reg(ioapic_idx, high_reg, (uint32_t)(entry >> 32));

  LOG_INFO("[APIC] IRQ %u -> GSI %u -> vec %u dest APIC %u%s%s", irq, gsi,
           vector, dest_apic_id, masked ? " [masked]" : "",
           is_level ? " [level]" : " [edge]");
}

void ioapic_mask_irq(uint8_t irq, int masked) {
  if (irq >= 16) {
    LOG_ERR("[APIC] ioapic_mask_irq: IRQ %u fuera de rango (0-15)", irq);
    return;
  }

  const acpi_info_t *acpi = acpi_get_info();
  if (acpi->ioapic_count == 0)
    return;

  uint8_t gsi = g_irq_to_gsi[irq];
  if (gsi == 0xFF)
    return; // IRQ no ruteable

  int ioapic_idx = -1;
  for (int i = 0; i < acpi->ioapic_count; i++) {
    const acpi_ioapic_t *cand = &acpi->ioapics[i];
    if (gsi >= cand->gsi_base && gsi < cand->gsi_base + 24) {
      ioapic_idx = i;
      break;
    }
  }
  if (ioapic_idx < 0 || !g_ioapics[ioapic_idx])
    return;

  uint32_t gsi_in_ioapic = gsi - acpi->ioapics[ioapic_idx].gsi_base;
  uint32_t low_reg = 0x10 + gsi_in_ioapic * 2;
  uint32_t low = ioapic_read_reg(ioapic_idx, low_reg);
  if (masked)
    low |= (uint32_t)IOAPIC_REDIR_MASKED;
  else
    low &= ~(uint32_t)IOAPIC_REDIR_MASKED;
  ioapic_write_reg(ioapic_idx, low_reg, low);
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

  LOG_INFO("  IRQ → GSI:");
  for (int i = 0; i < 16; i++) {
    if (g_irq_to_gsi[i] == 0xFF) {
      LOG_INFO("    IRQ %u -> (no ruteable)", i);
    } else {
      LOG_INFO("    IRQ %u -> GSI %u", i, g_irq_to_gsi[i]);
    }
  }
}

void *lapic_get_base(void) { return (void *)g_lapic; }
void apic_init_ap(void) {
  uint64_t apic_base = rdmsr(0x1B);
  apic_base |= (1ULL << 11);
  wrmsr(0x1B, apic_base);

  lapic_write(LAPIC_REG_SVR, 0xFF | LAPIC_SVR_ENABLE);
  lapic_write(LAPIC_REG_TPR, 0);

  lapic_write(LAPIC_REG_LVT_ERROR, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_PERF, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_REG_LVT_THERMAL, LAPIC_LVT_MASKED);
}
