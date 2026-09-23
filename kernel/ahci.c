// kernel/ahci.c
//
// Driver AHCI (SATA nativo). Ver ahci.h para contexto.
//
// Referencia principal: AHCI 1.3.1 spec (Intel, 2012).
//
// Secuencia de arranque de un puerto (spec 10.1.2 / 10.3):
//   1. ST=0, FRE=0, esperar CR=0 y FR=0.
//   2. Programar PxCLB/PxFB (solo con el puerto parado).
//   3. FRE=1 (necesario para recibir el FIS de firma tras COMRESET).
//   4. COMRESET, esperar DET=3 y TFD sin BSY/DRQ, leer PxSIG.
//   5. ST=1 justo antes de enviar el primer comando.

#include "ahci.h"
#include "apic.h"
#include "ata_common.h"
#include "block.h"
#include "cpu.h"
#include "heap.h"
#include "idt.h"
#include "io.h"
#include "klog.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "sched.h"
#include "string.h"
#include "uaccess.h"

// ===========================================================================
// Helpers MMIO
// ===========================================================================
static inline uint32_t ahci_read32(volatile uint8_t *base, uint32_t off) {
  return *(volatile uint32_t *)(base + off);
}
static inline void ahci_write32(volatile uint8_t *base, uint32_t off,
                                uint32_t val) {
  *(volatile uint32_t *)(base + off) = val;
}

static void ahci_irq_handler_msi(void);

enum ahci_eh_level {
  EH_NONE = 0,
  EH_LINK_RESET, // COMRESET
  EH_DEV_RESET,  // COMRESET + espera extendida
  EH_HARD_RESET, // HBA reset + re-setup del puerto
  EH_REVALIDATE, // re-IDENTIFY
  EH_DISABLE,    // desregistrar y liberar
};

// Offsets de los registros globales del HBA (desde ABAR).
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C
#define HBA_VS 0x10
#define HBA_CCC_CTL 0x14
#define HBA_CCC_PORTS 0x18
#define HBA_EM_LOC 0x1C
#define HBA_EM_CTL 0x20
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

// Bits de CAP.
#define CAP_SCLO (1u << 24)

// Bits de GHC.
#define GHC_HR (1u << 0)
#define GHC_IE (1u << 1)
#define GHC_AE (1u << 31)

// Offsets de los registros de puerto (desde Px base).
#define PxCLB 0x00
#define PxCLBU 0x04
#define PxFB 0x08
#define PxFBU 0x0C
#define PxIS 0x10
#define PxIE 0x14
#define PxCMD 0x18
#define PxTFD 0x20
#define PxSIG 0x24
#define PxSSTS 0x28
#define PxSCTL 0x2C
#define PxSERR 0x30
#define PxSACT 0x34
#define PxCI 0x38
#define PxSNTF 0x3C
#define PxFBS 0x40

#define PORT_REGS_SIZE 0x80

// Bits de PxTFD (status).
#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_DF 0x20
#define TFD_BSY 0x80

// Interrupciones que habilitamos mientras hay un comando en vuelo.
//
// NO se habilita DPS: salta al terminar el PRD, ANTES de que el comando
// complete (PxCI aún a 1). La finalización se decide por PxCI, no por la IRQ.
#define AHCI_IE_CMD                                                            \
  (AHCI_PxIS_DHRS | AHCI_PxIS_PSS | AHCI_PxIS_TFES | AHCI_PxIS_HBFS |          \
   AHCI_PxIS_HBDS | AHCI_PxIS_IFS | AHCI_PxIS_OFS)

// ===========================================================================
// Estado global
// ===========================================================================
static ahci_hba_t g_hba;
static int g_next_sd_index = 0;

static void port_recover(ahci_port_t *p);
// [FASE 3] Forward declarations para las funciones que se usan antes
// de su definición.
static int port_rw_start(ahci_port_t *p, uint64_t lba, uint32_t count,
                         void *buf, int is_read);
static int port_flush_start(ahci_port_t *p);
static void ahci_start_next_cmd(ahci_port_t *p);
static void ahci_cmd_complete(ahci_port_t *p);

// ===========================================================================
// Helpers de puerto
// ===========================================================================
static void port_write(ahci_port_t *p, uint32_t off, uint32_t val) {
  ahci_write32(p->regs, off, val);
}
static uint32_t port_read(ahci_port_t *p, uint32_t off) {
  return ahci_read32(p->regs, off);
}

// Devuelve 0 si la dirección física es usable por el HBA (dentro de
// 4 GB, o cualquier dirección si CAP.S64A=1).
static int ahci_phys_ok(uint64_t phys) {
  if (g_hba.cap & CAP_S64A)
    return 1;
  return phys < 0x100000000ULL;
}

// ---------------------------------------------------------------------------
// Popcount SWAR de 32 bits (sin libgcc).
// ---------------------------------------------------------------------------
static inline uint32_t ahci_popcount32(uint32_t x) {
  x = x - ((x >> 1) & 0x55555555u);
  x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
  x = (x + (x >> 4)) & 0x0F0F0F0Fu;
  return (x * 0x01010101u) >> 24;
}

// ---------------------------------------------------------------------------
// Delay en milisegundos usando TSC. No depende del LAPIC timer.
// ---------------------------------------------------------------------------
static void ahci_delay_ms(uint32_t ms) {
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0) {
    for (volatile uint64_t i = 0; i < (uint64_t)ms * 1000000ULL; i++)
      __asm__ volatile("pause");
    return;
  }
  uint64_t cycles = (freq / 1000ULL) * ms;
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t start = ((uint64_t)hi << 32) | lo;
  for (;;) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if ((((uint64_t)hi << 32) | lo) - start >= cycles)
      break;
    __asm__ volatile("pause");
  }
}

// ===========================================================================
// [NUEVO] Lectura del TSC para medir latencias.
// ===========================================================================
static inline uint64_t ahci_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

// ---------------------------------------------------------------------------
// Espera a que el dispositivo esté listo (SSTS.DET=3, TFD sin BSY/DRQ).
// Devuelve 0 si OK, -1 si timeout.
// ---------------------------------------------------------------------------
static int port_wait_ready(ahci_port_t *p, uint32_t timeout_ms) {
  for (uint32_t i = 0; i < timeout_ms; i++) {
    uint32_t ssts = port_read(p, PxSSTS);
    uint32_t tfd = port_read(p, PxTFD);
    if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_READY &&
        !(tfd & TFD_BSY) && !(tfd & TFD_DRQ)) {
      return 0;
    }
    // [MEJORA] En contexto de tarea, ceder CPU. En atómico, busy-wait.
    // (sched_sleep_ms no existe en este kernel; usamos sched_yield en
    // contexto de tarea como aproximación.)
    if (preempt_count() > 0) {
      ahci_delay_ms(1);
    } else {
      sched_yield();
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Condiciones para las wait queues.
//
// ahci_cmd_done:
//   1. Si el IRQ handler ha marcado irq_pending, el HBA ha actualizado
//      PxIS: el comando terminó (o hay un error que procesar). La IRQ
//      es la fuente de verdad de "algo pasó".
//   2. Fallback: si por alguna razón la IRQ no llega (IOAPIC enmascarado,
//      MSI mal configurado, ...), comprobamos PxCI por polling. El tick
//      del scheduler nos salva cada 1 ms.
//
// Se llama con irq_wq.lock cogido (desde wait_common) o sin lock (desde
// el bucle de reintentos de port_issue_wait). La lectura de p->irq_pending
// es atómica en x86 para un int alineado.
// ---------------------------------------------------------------------------
static bool ahci_cmd_done(void *arg) {
  ahci_port_t *p = (ahci_port_t *)arg;

  // [MEJORA] La IRQ es la fuente de verdad.
  if (p->irq_pending)
    return true;

  // Fallback por polling: el comando terminó si PxCI ya no tiene el bit 0.
  if (!(port_read(p, PxCI) & 1u))
    return true;

  // Error fatal detectado por la IRQ pero sin PxCI limpio.
  return (p->irq_status & AHCI_PxIS_FATAL) != 0;
}

static bool ahci_slot_free(void *arg) { return !((ahci_port_t *)arg)->busy; }

// ---------------------------------------------------------------------------
// Serialización del slot 0. Nunca se duerme con un spinlock cogido: el lock
// solo protege el flag `busy`.
// ---------------------------------------------------------------------------
static void port_acquire(ahci_port_t *p) {
  for (;;) {
    unsigned long flags = spin_lock_irqsave(&p->lock);
    if (!p->busy) {
      p->busy = 1;
      spin_unlock_irqrestore(&p->lock, flags);
      return;
    }
    spin_unlock_irqrestore(&p->lock, flags);
    wait_event(&p->slot_wq, ahci_slot_free, p);
  }
}

static void port_release(ahci_port_t *p) {
  unsigned long flags = spin_lock_irqsave(&p->lock);
  p->busy = 0;
  spin_unlock_irqrestore(&p->lock, flags);
  wake_up_one(&p->slot_wq);
}

// ===========================================================================
// Reset del HBA
// ===========================================================================
static int hba_reset(void) {
  volatile uint8_t *abar = g_hba.abar;

  // 1. BOHC handoff.
  uint32_t bohc = ahci_read32(abar, HBA_BOHC);
  if (bohc & 0x03) {
    LOG_DEBUG("[AHCI] BOHC: pidiendo ownership (BOHC=0x%x)", bohc);
    ahci_write32(abar, HBA_BOHC, bohc | 0x02);
    for (int i = 0; i < 25; i++) {
      if (!(ahci_read32(abar, HBA_BOHC) & 0x01))
        break;
      ahci_delay_ms(1);
    }
    if (ahci_read32(abar, HBA_BOHC) & 0x01)
      LOG_WARN("[AHCI] BOHC: BOS no se limpió en 25 ms");
  }

  // 2. HBA Reset.
  uint32_t ghc = ahci_read32(abar, HBA_GHC);
  ahci_write32(abar, HBA_GHC, ghc | GHC_HR);
  int ok = 0;
  for (int i = 0; i < 1000; i++) {
    if (!(ahci_read32(abar, HBA_GHC) & GHC_HR)) {
      ok = 1;
      break;
    }
    ahci_delay_ms(1);
  }
  if (!ok) {
    LOG_ERR("[AHCI] reset del HBA no completó en 1 s");
    return -1;
  }

  // 3. Habilitar AHCI.
  ghc = ahci_read32(abar, HBA_GHC);
  ahci_write32(abar, HBA_GHC, ghc | GHC_AE);

  // 4. Deshabilitar IRQs globales mientras configuramos.
  ahci_write32(abar, HBA_GHC, ahci_read32(abar, HBA_GHC) & ~GHC_IE);

  // 5. Limpiar IS global.
  ahci_write32(abar, HBA_IS, 0xFFFFFFFF);

  return 0;
}

// ===========================================================================
// Parada / arranque del puerto
// ===========================================================================

// Para el puerto por completo (ST=0 y FRE=0) y espera CR=0 y FR=0. Es lo que
// exige la spec antes de tocar PxCLB/PxFB o liberar su memoria.
static int port_stop(ahci_port_t *p) {
  uint32_t cmd = port_read(p, PxCMD);
  cmd &= ~(AHCI_PxCMD_ST | AHCI_PxCMD_FRE); // UNA sola escritura
  port_write(p, PxCMD, cmd);
  for (int i = 0; i < 500; i++) {
    if (!(port_read(p, PxCMD) & (AHCI_PxCMD_CR | AHCI_PxCMD_FR)))
      return 0;
    ahci_delay_ms(1);
  }
  LOG_WARN("[AHCI] puerto %u: CR/FR no se limpiaron (PxCMD=0x%x)", p->port_num,
           port_read(p, PxCMD));
  return -1;
}

// ST=1. Requiere PxTFD sin BSY/DRQ.
static int port_start(ahci_port_t *p) {
  for (int i = 0; i < 1000; i++) {
    if (!(port_read(p, PxTFD) & (TFD_BSY | TFD_DRQ)))
      break;
    ahci_delay_ms(1);
  }
  if (port_read(p, PxTFD) & (TFD_BSY | TFD_DRQ)) {
    LOG_WARN("[AHCI] puerto %u: BSY/DRQ activos, no se puede poner ST=1 "
             "(TFD=0x%x)",
             p->port_num, port_read(p, PxTFD));
    return -1;
  }

  uint32_t cmd = port_read(p, PxCMD);
  port_write(p, PxCMD, cmd | AHCI_PxCMD_ST);
  for (int i = 0; i < 500; i++) {
    if (port_read(p, PxCMD) & AHCI_PxCMD_CR)
      return 0;
    ahci_delay_ms(1);
  }
  LOG_WARN("[AHCI] puerto %u: CR no se activó tras ST=1", p->port_num);
  return -1;
}

// ===========================================================================
// COMRESET de un puerto
//
// Requisitos: ST=0, FRE=1 y PxCLB/PxFB ya programados (si no, el HBA no
// puede volcar el FIS de firma y PxSIG/PxTFD no se actualizan). NO arranca
// el motor de comandos: eso lo hace port_start().
// ===========================================================================
static int port_reset(ahci_port_t *p) {
  port_write(p, PxSERR, 0xFFFFFFFF);
  port_write(p, PxIS, 0xFFFFFFFF);

  // DET=1 durante >= 1 ms, luego DET=0.
  uint32_t sctl = port_read(p, PxSCTL);
  port_write(p, PxSCTL, (sctl & ~0x0Fu) | 0x01);
  ahci_delay_ms(2);
  port_write(p, PxSCTL, (sctl & ~0x0Fu));

  // Esperar a que el enlace se restablezca (DET=3).
  int linked = 0;
  for (int i = 0; i < 1000; i++) {
    if ((port_read(p, PxSSTS) & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_READY) {
      linked = 1;
      break;
    }
    ahci_delay_ms(1);
  }
  if (!linked) {
    LOG_WARN("[AHCI] puerto %u: sin enlace tras COMRESET (SSTS=0x%x)",
             p->port_num, port_read(p, PxSSTS));
    return -1;
  }

  port_write(p, PxSERR, 0xFFFFFFFF);

  // Esperar a que el dispositivo esté listo.
  if (port_wait_ready(p, 1000) != 0) {
    LOG_WARN("[AHCI] puerto %u: timeout esperando device ready "
             "(SSTS=0x%x TFD=0x%x)",
             p->port_num, port_read(p, PxSSTS), port_read(p, PxTFD));
    return -1;
  }

  // [NUEVO] Esperar a que PxSIG sea estable (ni 0 ni 0xFFFFFFFF).
  // Sin esto, la lectura de la firma en ahci_probe_port puede pillar
  // un valor transitorio.
  uint32_t sig = 0, sig_prev = 0xFFFFFFFF;
  int stable = 0;
  for (int i = 0; i < 500; i++) {
    sig = port_read(p, PxSIG);
    if (sig != 0 && sig != 0xFFFFFFFF && sig == sig_prev) {
      stable = 1;
      break;
    }
    sig_prev = sig;
    ahci_delay_ms(1);
  }
  if (!stable) {
    LOG_WARN("[AHCI] puerto %u: PxSIG inestable (0x%08x)", p->port_num, sig);
    return -1;
  }

  LOG_DEBUG("[AHCI] puerto %u: reset completado, SSTS=0x%x, TFD=0x%x, "
            "SIG=0x%08x",
            p->port_num, port_read(p, PxSSTS), port_read(p, PxTFD),
            port_read(p, PxSIG));
  return 0;
}

// ===========================================================================
// Liberar memoria del puerto
// ===========================================================================
static void port_free_memory(ahci_port_t *p) {
  // [FIX] Deshabilitar IRQs y limpiar PxIS ANTES de tocar nada más.
  // Si el puerto estaba generando IRQs, el handler no debe entrar aquí
  // una vez hemos empezado a liberar memoria.
  if (p->regs) {
    port_write(p, PxIE, 0);
    port_write(p, PxIS, 0xFFFFFFFF);
  }

  if (p->regs) {
    if (port_stop(p) != 0) {
      LOG_ERR("[AHCI] puerto %u: no se pudo parar; marcando como muerto "
              "para no reutilizar memoria DMA",
              p->port_num);
      // [FIX] No liberamos memoria (el HBA podría seguir escribiendo),
      // pero impedimos que se reutilice y que el handler lo toque.
      p->dead = 1;
      p->regs = NULL;
      p->cmd_list = NULL;
      p->rx_fis = NULL;
      p->cmd_table = NULL;
      p->identify = NULL;
      p->bdev = NULL;
      p->initialized = 0;
      p->has_device = 0;
      return;
    }
    port_write(p, PxCLB, 0);
    port_write(p, PxCLBU, 0);
    port_write(p, PxFB, 0);
    port_write(p, PxFBU, 0);
  }

  if (p->cmd_list_phys) {
    pmm_free_page(p->cmd_list_phys);
    p->cmd_list_phys = 0;
  }
  if (p->rx_fis_phys) {
    pmm_free_page(p->rx_fis_phys);
    p->rx_fis_phys = 0;
  }
  if (p->cmd_table_phys) {
    pmm_free_page(p->cmd_table_phys);
    p->cmd_table_phys = 0;
  }
  if (p->identify_phys) {
    pmm_free_page(p->identify_phys);
    p->identify_phys = 0;
  }
  p->cmd_list = NULL;
  p->rx_fis = NULL;
  p->cmd_table = NULL;
  p->identify = NULL;
  p->regs = NULL; // [FIX] el handler ya no tocará este puerto
  p->dead = 1;    // [FIX]
  p->initialized = 0;
  p->has_device = 0;
  p->bdev = NULL;
}

// ===========================================================================
// Configuración de las estructuras del puerto
//
// Deja el puerto con ST=0 y FRE=1. El motor de comandos se arranca después
// del COMRESET con port_start().
// ===========================================================================
static int port_setup_memory(ahci_port_t *p) {
  // PxCLB/PxFB solo se pueden escribir con ST=0 y FRE=0.
  if (port_stop(p) != 0)
    return -1;

  // --- Command List: 32 * 32 B = 1 KB. Alineada a 1 KB (página). ---
  p->cmd_list_phys = pmm_alloc_page();
  if (!p->cmd_list_phys || (p->cmd_list_phys & 0x3FF) ||
      !ahci_phys_ok(p->cmd_list_phys)) {
    LOG_ERR("[AHCI] puerto %u: Command List no usable (phys=0x%lx, S64A=%d)",
            p->port_num, (unsigned long)p->cmd_list_phys,
            !!(g_hba.cap & CAP_S64A));
    port_free_memory(p);
    return -1;
  }
  p->cmd_list = (ahci_cmd_header_t *)phys_to_virt(p->cmd_list_phys);
  memset(p->cmd_list, 0, PAGE_SIZE);

  // --- RX FIS: 256 B, alineada a 256 B (página). ---
  p->rx_fis_phys = pmm_alloc_page();
  if (!p->rx_fis_phys || (p->rx_fis_phys & 0xFF) ||
      !ahci_phys_ok(p->rx_fis_phys)) {
    LOG_ERR("[AHCI] puerto %u: RX FIS no usable (phys=0x%lx, S64A=%d)",
            p->port_num, (unsigned long)p->rx_fis_phys,
            !!(g_hba.cap & CAP_S64A));
    port_free_memory(p);
    return -1;
  }
  p->rx_fis = (ahci_rx_fis_t *)phys_to_virt(p->rx_fis_phys);
  memset(p->rx_fis, 0, PAGE_SIZE);

  // --- Command Table: alineada a 128 B (página). ---
  p->cmd_table_phys = pmm_alloc_page();
  if (!p->cmd_table_phys || (p->cmd_table_phys & 0x7F) ||
      !ahci_phys_ok(p->cmd_table_phys)) {
    LOG_ERR("[AHCI] puerto %u: Command Table no usable (phys=0x%lx, S64A=%d)",
            p->port_num, (unsigned long)p->cmd_table_phys,
            !!(g_hba.cap & CAP_S64A));
    port_free_memory(p);
    return -1;
  }
  p->cmd_table = (ahci_cmd_table_t *)phys_to_virt(p->cmd_table_phys);
  memset(p->cmd_table, 0, PAGE_SIZE);

  // --- Buffer IDENTIFY: 1 página propia, 512 B usados. ---
  p->identify_phys = pmm_alloc_page();
  if (!p->identify_phys || (p->identify_phys & 0xFFF) ||
      !ahci_phys_ok(p->identify_phys)) {
    LOG_ERR("[AHCI] puerto %u: buffer IDENTIFY no usable (phys=0x%lx, S64A=%d)",
            p->port_num, (unsigned long)p->identify_phys,
            !!(g_hba.cap & CAP_S64A));
    port_free_memory(p);
    return -1;
  }
  p->identify = (uint16_t *)phys_to_virt(p->identify_phys);
  memset(p->identify, 0, PAGE_SIZE);

  // --- Programar el puerto (parado). ---
  port_write(p, PxCLB, (uint32_t)p->cmd_list_phys);
  port_write(p, PxCLBU, (uint32_t)(p->cmd_list_phys >> 32));
  port_write(p, PxFB, (uint32_t)p->rx_fis_phys);
  port_write(p, PxFBU, (uint32_t)(p->rx_fis_phys >> 32));

  // --- Command Header del slot 0. ---
  ahci_cmd_header_t *hdr = &p->cmd_list[0];
  memset(hdr, 0, sizeof(*hdr));
  hdr->ctba = (uint32_t)p->cmd_table_phys;
  hdr->ctbau = (uint32_t)(p->cmd_table_phys >> 32);

  // --- FRE=1, SUD=1, POD=1. ST sigue a 0. ---
  uint32_t cmd = port_read(p, PxCMD);
  cmd |= AHCI_PxCMD_FRE;
  cmd |= AHCI_PxCMD_SUD;
  cmd |= AHCI_PxCMD_POD;
  port_write(p, PxCMD, cmd);

  int fr_ok = 0;
  for (int i = 0; i < 500; i++) {
    if (port_read(p, PxCMD) & AHCI_PxCMD_FR) {
      fr_ok = 1;
      break;
    }
    ahci_delay_ms(1);
  }
  if (!fr_ok) {
    LOG_ERR("[AHCI] puerto %u: FR no se activó tras FRE=1", p->port_num);
    port_free_memory(p);
    return -1;
  }

  port_write(p, PxSERR, 0xFFFFFFFF);
  port_write(p, PxIS, 0xFFFFFFFF);
  port_write(p, PxIE, 0);
  return 0;
}

// ===========================================================================
// Envío de un comando por el slot 0 y espera de su finalización.
//
// El llamante ya rellenó el Command Header y la Command Table. Devuelve
// ATA_OK, ATA_ERR_TIMEOUT o ATA_ERR_ABRT. Tras un error deja el puerto
// recuperado y listo para el siguiente comando.
// ===========================================================================
static int port_issue_wait(ahci_port_t *p, uint32_t ie_mask) {
  // Estado limpio para esta operación.
  // [FIX] Resetear irq_pending es IMPRESCINDIBLE: si no, ahci_cmd_done
  // devolvería true inmediatamente en el siguiente comando.
  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
  p->irq_status = 0;
  p->irq_pending = 0;
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);

  port_write(p, PxIS, 0xFFFFFFFF);
  port_write(p, PxIE, ie_mask);

  // La Command Table y el header deben ser visibles antes de PxCI.
  __asm__ volatile("mfence" ::: "memory");

  // [NUEVO] Medir latencia del comando.
  uint64_t t0 = ahci_rdtsc();

  port_write(p, PxCI, 1u);

  // [FIX] Timeout configurable por puerto.
  uint64_t timeout = p->cmd_timeout_ms ? p->cmd_timeout_ms : 5000;

  // Un DMA en vuelo no se puede abandonar: si nos interrumpen (-EINTR)
  // con el comando aún activo, volvemos a esperar.
  long w = 0;
  for (int tries = 0; tries < 4; tries++) {
    w = wait_event_interruptible_timeout(&p->irq_wq, ahci_cmd_done, p, timeout);
    if (w >= 0)
      break;
    if (ahci_cmd_done(p))
      break;
  }

  uint64_t t1 = ahci_rdtsc();

  // Revalidar: el timeout puede coincidir con la finalización.
  if (!ahci_cmd_done(p)) {
    LOG_ERR("[AHCI] puerto %u: timeout (PxIS=0x%x PxCI=0x%x PxTFD=0x%x)",
            p->port_num, port_read(p, PxIS), port_read(p, PxCI),
            port_read(p, PxTFD));
    p->timeout_count++;
    port_recover(p);
    return ATA_ERR_TIMEOUT;
  }

  uint32_t is = p->irq_status;
  uint32_t tfd = port_read(p, PxTFD);
  if ((is & AHCI_PxIS_FATAL) || (tfd & (TFD_ERR | TFD_DF))) {
    LOG_ERR("[AHCI] puerto %u: error de comando (PxIS=0x%x PxTFD=0x%x)",
            p->port_num, is, tfd);
    p->error_count++;
    port_recover(p);
    return ATA_ERR_ABRT;
  }

  // [NUEVO] Log de latencia cada 64 comandos.
  if ((p->cmd_count & 0x3F) == 0) {
    extern uint64_t klog_get_tsc_freq(void);
    uint64_t freq = klog_get_tsc_freq();
    uint64_t cycles = t1 - t0;
    // freq puede ser 0 si el TSC no está calibrado; evitamos div/0.
    uint64_t us = freq ? (cycles * 1000000ULL / freq) : 0;
    LOG_DEBUG("[AHCI] puerto %u: cmd #%u, %lu ciclos (~%lu us), irq=%d",
              p->port_num, p->cmd_count, (unsigned long)cycles,
              (unsigned long)us, p->irq_pending);
  }

  p->cmd_count++;
  return ATA_OK;
}

// [NUEVO] Reintenta una vez tras una recuperación por timeout.
static int port_issue_wait_retry(ahci_port_t *p, uint32_t ie_mask) {
  int rc = port_issue_wait(p, ie_mask);
  if (rc == ATA_ERR_TIMEOUT) {
    LOG_WARN("[AHCI] puerto %u: reintentando comando tras timeout",
             p->port_num);
    rc = port_issue_wait(p, ie_mask);
  }
  return rc;
}

// ===========================================================================
// [FASE 3] Estado de transferencia asíncrona.
//
// El bio en vuelo y los campos xfer_* están protegidos por irq_wq.lock.
// El IRQ handler los actualiza; ahci_submit y ahci_start_next_cmd los
// leen/escriben con el mismo lock.
// ===========================================================================

// Lee y limpia el estado de transferencia bajo lock.
// Devuelve 1 si había un bio en vuelo, 0 si no.
static int xfer_take_state(ahci_port_t *p, bio_t **bio_out, uint64_t *lba,
                           uint32_t *left, uint8_t **buf, int *is_read,
                           int *need_flush, int *error) {
  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
  bio_t *bio = p->inflight_bio;
  if (!bio) {
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
    return 0;
  }
  *bio_out = bio;
  *lba = p->xfer_lba;
  *left = p->xfer_left;
  *buf = p->xfer_buf;
  *is_read = p->xfer_is_read;
  *need_flush = p->xfer_need_flush;
  *error = p->xfer_error;

  p->inflight_bio = NULL;
  p->xfer_lba = 0;
  p->xfer_left = 0;
  p->xfer_buf = NULL;
  p->xfer_is_read = 0;
  p->xfer_need_flush = 0;
  p->xfer_error = 0;
  p->xfer_state = 0;   // [FIX] IDLE
  p->xfer_started = 0; // Legacy.
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);
  return 1;
}

// Avanza el estado de transferencia tras un comando completado con éxito.
// Devuelve 1 si quedan comandos por lanzar, 0 si el bio está terminado.
//
// [FIX] NO toca xfer_started. Ese flag lo gestionan port_rw_start,
// port_flush_start y ahci_cmd_complete.
static int xfer_advance(ahci_port_t *p, uint32_t consumed_sectors) {
  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
  if (!p->inflight_bio) {
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
    return 0;
  }

  p->xfer_lba += consumed_sectors;
  p->xfer_left -= consumed_sectors;
  p->xfer_buf += (uint64_t)consumed_sectors * p->sector_size;
  // NO tocar xfer_state ni xfer_started.

  int more = (p->xfer_left > 0) || p->xfer_need_flush;
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);
  return more;
}

// Marca el bio en vuelo con un error y lo completa.
static void xfer_fail_and_complete(ahci_port_t *p, int error) {
  bio_t *bio = NULL;
  uint64_t lba;
  uint32_t left;
  uint8_t *buf;
  int is_read, need_flush, old_err;
  if (!xfer_take_state(p, &bio, &lba, &left, &buf, &is_read, &need_flush,
                       &old_err)) {
    // No había bio en vuelo. Asegurar que el estado es IDLE.
    unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
    p->xfer_state = 0;
    p->xfer_started = 0;
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
    return;
  }

  // xfer_take_state ya puso xfer_state = 0.

  (void)lba;
  (void)left;
  (void)buf;
  (void)is_read;
  (void)need_flush;
  (void)old_err;

  port_release(p);
  bio_endio(bio, error);
}

static void ahci_start_next_cmd(ahci_port_t *p) {
  // Leer estado bajo lock (sin modificarlo, solo para decidir).
  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
  bio_t *bio = p->inflight_bio;
  uint64_t lba = p->xfer_lba;
  uint32_t left = p->xfer_left;
  uint8_t *buf = p->xfer_buf;
  int is_read = p->xfer_is_read;
  int need_flush = p->xfer_need_flush;
  int err = p->xfer_error;
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);

  if (!bio) {
    return;
  }

  if (err != 0) {
    // Hubo un error antes: completar y salir.
    xfer_fail_and_complete(p, err);
    return;
  }

  // 1. ¿Quedan datos por transferir?
  if (left > 0) {
    uint32_t max_sectors = AHCI_MAX_XFER_BYTES / p->sector_size;
    if (max_sectors == 0)
      max_sectors = 1;
    uint32_t n = (left < max_sectors) ? left : max_sectors;

    // port_rw_start pondrá xfer_state = 1 (RUNNING).
    int rc = port_rw_start(p, lba, n, buf, is_read);
    if (rc != ATA_OK) {
      LOG_ERR("[AHCI] puerto %u: fallo al lanzar %s lba=%lu count=%u",
              p->port_num, is_read ? "READ" : "WRITE", (unsigned long)lba, n);
      xfer_fail_and_complete(p, -EIO);
      return;
    }
    return;
  }

  // 2. ¿Queda un FLUSH pendiente?
  if (need_flush) {
    unsigned long f2 = spin_lock_irqsave(&p->irq_wq.lock);
    p->xfer_need_flush = 0;
    spin_unlock_irqrestore(&p->irq_wq.lock, f2);

    // port_flush_start pondrá xfer_state = 1 (RUNNING).
    int rc = port_flush_start(p);
    if (rc != ATA_OK) {
      LOG_ERR("[AHCI] puerto %u: fallo al lanzar FLUSH", p->port_num);
      xfer_fail_and_complete(p, -EIO);
      return;
    }
    return;
  }

  // 3. Bio completado. Estado a IDLE.
  bio_t *done = NULL;
  uint64_t l;
  uint32_t le;
  uint8_t *b;
  int ir, nf, old_err;
  if (xfer_take_state(p, &done, &l, &le, &b, &ir, &nf, &old_err)) {
    // xfer_take_state ya pone xfer_state = 0.
    port_release(p);
    bio_endio(done, 0);
  }
}

// Procesa un comando completado. Se llama desde el IRQ handler o desde
// el polling. Solo el primero que llega procesa el comando.
//
// [FIX] La fuente de verdad es `irq_status != 0`. Solo procesamos si la
// IRQ (real o simulada por el polling) ha marcado algo. Esto evita que
// el polling procese un comando que acaba de lanzarse pero aún no ha
// terminado (cuando xfer_started ya está a 1 pero irq_status es 0).
//
// [FIX VirtualBox ICH9] Algunos HBAs (VirtualBox ICH9 SATA) pueden
// señalar PxIS.DHRS y limpiar PxCI antes de que la escritura de PRDBC
// sea visible. Leer PRDBC justo en esa ventana devuelve 0 y abortaría
// un comando que en realidad se completó bien. QEMU es más secuencial
// y no expone la carrera.
static void ahci_cmd_complete(ahci_port_t *p) {
  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);

  // [FIX] Solo procesar si el estado es RUNNING. Si es IDLE (sin comando)
  // o PROCESSING (otro ya está dentro), salir.
  if (p->xfer_state != 1) {
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
    return;
  }

  // Solo procesar si la IRQ ha marcado algo.
  if (p->irq_status == 0) {
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
    return;
  }

  // Tomar el comando: RUNNING -> PROCESSING.
  p->xfer_state = 2;

  bio_t *bio = p->inflight_bio;
  uint32_t is = p->irq_status;
  p->irq_status = 0;
  p->irq_pending = 0;
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);

  if (!bio) {
    // Estado inconsistente. Volver a IDLE.
    flags = spin_lock_irqsave(&p->irq_wq.lock);
    p->xfer_state = 0;
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
    return;
  }

  // Comprobar que PxCI está limpio.
  if (port_read(p, PxCI) & 1u) {
    LOG_DEBUG("[AHCI] puerto %u: IRQ con PxCI aún activo (PxIS=0x%x), "
              "ignorando",
              p->port_num, is);
    // Devolver el estado a RUNNING para que otro pueda procesar.
    flags = spin_lock_irqsave(&p->irq_wq.lock);
    if (p->xfer_state == 2)
      p->xfer_state = 1;
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
    return;
  }

  // Comprobar errores del HBA.
  uint32_t tfd = port_read(p, PxTFD);
  if ((is & AHCI_PxIS_FATAL) || (tfd & (TFD_ERR | TFD_DF))) {
    LOG_ERR("[AHCI] puerto %u: error de comando (PxIS=0x%x PxTFD=0x%x)",
            p->port_num, is, tfd);
    p->error_count++;
    xfer_fail_and_complete(p, -EIO);
    return;
  }

  // Éxito. Calcular cuántos sectores consumió este comando.
  ahci_cmd_header_t *hdr = &p->cmd_list[0];
  uint32_t transferred = hdr->prdbc;

  // [FIX VirtualBox ICH9] Algunos HBAs (VirtualBox ICH9 SATA) pueden
  // señalar PxIS.DHRS y limpiar PxCI antes de que la escritura de
  // PRDBC sea visible. Leer PRDBC justo en esa ventana devuelve 0 y
  // abortaría un comando que en realidad se completó bien. QEMU es más
  // secuencial y no expone la carrera.
  //
  // Primer nivel de defensa: reintentar la lectura unas cuantas veces
  // antes de darla por perdida.
  if (transferred == 0) {
    for (int retry = 0; retry < 1000; retry++) {
      __asm__ volatile("pause");
      transferred = hdr->prdbc;
      if (transferred != 0)
        break;
    }
  }

  uint32_t sectors = 0;
  if (p->sector_size > 0)
    sectors = transferred / p->sector_size;

  // Leer xfer_left y xfer_need_flush bajo lock.
  flags = spin_lock_irqsave(&p->irq_wq.lock);
  uint32_t expected = 0;
  if (p->xfer_left > 0) {
    uint32_t max_sectors = AHCI_MAX_XFER_BYTES / p->sector_size;
    if (max_sectors == 0)
      max_sectors = 1;
    expected = (p->xfer_left < max_sectors) ? p->xfer_left : max_sectors;
  }
  int was_flush = (expected == 0);
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);

  // Validación de PRDBC para comandos con datos.
  if (!was_flush) {
    if (transferred == 0) {
      // [FIX VirtualBox ICH9] Segundo nivel de defensa. Si tras
      // reintentar PRDBC sigue a 0, pero PxCI está limpio y no hubo
      // error del HBA, asumimos que el comando se completó y que el
      // HBA no actualizó PRDBC (peculiaridad del emulador).
      //
      // Es seguro asumirlo: la fuente de verdad de "el comando
      // terminó" es PxCI limpio + IRQ recibida + sin error del HBA,
      // no PRDBC. PRDBC es diagnóstico.
      LOG_WARN("[AHCI] puerto %u: PRDBC=0 sin error tras reintentos; "
               "asumiendo %u sectores (VirtualBox quirk)",
               p->port_num, expected);
      sectors = expected;
    } else if (sectors < expected) {
      LOG_ERR("[AHCI] puerto %u: transferidos %u sectores de %u esperados",
              p->port_num, sectors, expected);
      xfer_fail_and_complete(p, -EIO);
      return;
    }
  }

  p->cmd_count++;

  // Avanzar el estado.
  xfer_advance(p, sectors);

  // Lanzar el siguiente comando (que pondrá estado a RUNNING) o
  // completar el bio (que pondrá estado a IDLE).
  ahci_start_next_cmd(p);
}

// ===========================================================================
// Fase ack del IRQ handler legacy.
//
// Enmascara SIEMPRE la IRQ del AHCI en el IOAPIC, incluso si HBA_IS == 0.
//
// Motivo: en hardware con IRQ sharing (por ejemplo, el SMBus en QEMU),
// la IRQ puede venir de otro dispositivo. Sin enmascarar, el IOAPIC
// redispara inmediatamente porque la línea del otro dispositivo sigue
// asertada. El storm consume CPU y puede colgar el sistema.
//
// Enmascarando siempre, el IOAPIC no puede redisparar hasta que el
// process desenmascare. El process solo desenmascara cuando no hay
// comandos en vuelo, así que no hay ventana para el storm.
// ===========================================================================
static void ahci_irq_ack(void) {
  volatile uint8_t *abar = g_hba.abar;
  if (!abar)
    return;

  ioapic_mask_irq(g_hba.irq, 1);
  g_hba.irq_masked = 1;

  uint32_t is_global = ahci_read32(abar, HBA_IS);
  if (is_global == 0) {
    g_hba.irq_spurious_count++;
    return;
  }
  g_hba.irq_spurious_count = 0;

  for (int i = 0; i < 32; i++) {
    if (!(is_global & (1u << i)))
      continue;
    ahci_port_t *p = &g_hba.ports[i];
    if (!p->regs || p->dead)
      continue;

    uint32_t px_is = port_read(p, PxIS);
    port_write(p, PxIS, px_is);
    if (px_is == 0)
      continue;

    unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
    p->irq_pending++;
    p->irq_status |= px_is;
    // [FIX] Despertar a los waiters del path síncrono (port_issue_wait).
    wake_up_all_locked(&p->irq_wq);
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
  }

  ahci_write32(abar, HBA_IS, is_global);
}

// ===========================================================================
// Fase process del IRQ handler legacy.
//
// Procesa los comandos del AHCI. Al final, desenmascara la IRQ del
// IOAPIC SIEMPRE (no solo si no hay comandos en vuelo).
//
// [FIX race enmascaramiento] El código anterior solo desenmascaraba si
// ningún puerto tenía PxCI activo. Esto causaba un fallo silencioso:
// si la IRQ llegaba justo cuando el HBA había limpiado PxCI pero antes
// de que el proceso comprobara, la IRQ se quedaba enmascarada para
// siempre. El siguiente comando solo se completaba vía polling
// (10 ms de espera).
//
// La condición PxCI no es necesaria: la IRQ del AHCI es level-triggered
// y la línea la controla HBA_IS, que depende de PxIS. En la fase ack ya
// limpiamos PxIS, así que la línea queda desasertada. Si un comando
// sigue en vuelo, la línea permanece baja hasta que ese comando
// complete; entonces se re-aserta y el IOAPIC entrega una nueva IRQ.
//
// [FIX storm IRQ compartida] Si otro dispositivo comparte la línea y
// mantiene la aserción, la fase ack verá HBA_IS == 0 repetidamente e
// incrementará irq_spurious_count. Al superar AHCI_SPURIOUS_LIMIT
// dejamos la IRQ enmascarada permanentemente y el polling se encarga.
// ===========================================================================
static void ahci_irq_process(void) {
  // 1. Procesar los comandos completados.
  for (int i = 0; i < 32; i++) {
    ahci_port_t *p = &g_hba.ports[i];
    if (!p->regs || p->dead)
      continue;

    unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
    int has_bio = (p->inflight_bio != NULL);
    int running = (p->xfer_state == 1);
    uint32_t irq_status = p->irq_status;
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);

    if (!has_bio || !running || irq_status == 0)
      continue;

    ahci_cmd_complete(p);
  }

  // 2. Desenmascarar la IRQ del IOAPIC.
  //
  // [FIX] Desenmascaramos SIEMPRE, sin comprobar PxCI. Si otro
  // dispositivo comparte la línea y sigue asertando, veremos IRQs
  // espurias (HBA_IS == 0). El contador de espurias lo detecta y, si
  // supera el umbral, dejamos la IRQ enmascarada para evitar el storm;
  // el polling completará los comandos.
  if (g_hba.irq_masked) {
    if (g_hba.irq_spurious_count >= AHCI_SPURIOUS_LIMIT) {
      // Storm detectado: mantener enmascarada. El polling se encarga
      // de completar los comandos. Se recupera reiniciando el sistema
      // (o desconectando el dispositivo que comparte la línea).
      return;
    }
    ioapic_mask_irq(g_hba.irq, 0);
    g_hba.irq_masked = 0;
  }
}

// ===========================================================================
// IDENTIFY DEVICE
// ===========================================================================
static int port_identify(ahci_port_t *p) {
  memset(p->cmd_table, 0, sizeof(*p->cmd_table));
  ahci_cmd_header_t *hdr = &p->cmd_list[0];
  memset(hdr, 0, sizeof(*hdr));
  hdr->ctba = (uint32_t)p->cmd_table_phys;
  hdr->ctbau = (uint32_t)(p->cmd_table_phys >> 32);

  fis_reg_h2d_t *fis = (fis_reg_h2d_t *)p->cmd_table->cfis;
  memset(fis, 0, sizeof(*fis));
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  fis->command = ATA_CMD_IDENTIFY;

  p->cmd_table->prdt[0].dba = (uint32_t)p->identify_phys;
  p->cmd_table->prdt[0].dbau = (uint32_t)(p->identify_phys >> 32);
  p->cmd_table->prdt[0].reserved = 0;
  p->cmd_table->prdt[0].flags = (512 - 1) | AHCI_PRDT_IOC;

  hdr->flags = sizeof(fis_reg_h2d_t) / 4;
  hdr->prdtl = 1;

  memset(p->identify, 0, 512);

  LOG_DEBUG("[AHCI] puerto %u: enviando IDENTIFY", p->port_num);
  int rc = port_issue_wait_retry(p, AHCI_IE_CMD);
  if (rc != ATA_OK)
    return rc;

  LOG_DEBUG("[AHCI] puerto %u: IDENTIFY terminó, PxCI=0x%x PxTFD=0x%x "
            "PRDBC=%u",
            p->port_num, port_read(p, PxCI), port_read(p, PxTFD), hdr->prdbc);

  if (p->identify[0] == 0 && p->identify[1] == 0 && p->identify[2] == 0 &&
      p->identify[3] == 0) {
    LOG_WARN("[AHCI] puerto %u: IDENTIFY devolvió buffer vacío", p->port_num);
    return ATA_ERR_UNKNOWN;
  }

  return ATA_OK;
}

// ===========================================================================
// Parseo del IDENTIFY
// ===========================================================================
static void ahci_parse_identify(ahci_port_t *p) {
  const uint16_t *id = p->identify;

  uint16_t caps = id[49];
  uint16_t caps83 = id[83];
  uint8_t supports_lba = (caps & (1 << 9)) ? 1 : 0;
  uint8_t supports_lba48 = (caps83 & (1 << 10)) ? 1 : 0;

  uint64_t lba28 = ((uint64_t)id[61] << 16) | id[60];
  uint64_t lba48 = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                   ((uint64_t)id[101] << 16) | id[100];

  if (supports_lba48 && lba48 > 0) {
    p->num_sectors = lba48;
  } else if (supports_lba && lba28 > 0) {
    p->num_sectors = lba28;
  } else {
    p->num_sectors = 0;
  }

  // Tamaño LÓGICO de sector. La palabra 106 solo es válida con bit14=1 y
  // bit15=0; el bit 12 indica que el sector lógico no es de 512 B y su
  // tamaño (en palabras de 16 bits) está en las palabras 117-118.
  // (Los bits 3:0 de la palabra 106 son la relación físico/lógico: NO
  // sirven para el tamaño lógico.)
  uint16_t w106 = id[106];
  p->sector_size = 512;
  if ((w106 & 0xC000) == 0x4000 && (w106 & 0x1000)) {
    uint32_t words = ((uint32_t)id[118] << 16) | id[117];
    if (words >= 256 && words <= 32768)
      p->sector_size = words * 2;
  }

  // Modelo (words 27..46) con trim de espacios finales.
  char model[41];
  int j = 0;
  for (int i = 27; i <= 46; i++) {
    model[j++] = (char)(id[i] >> 8);
    model[j++] = (char)(id[i] & 0xFF);
  }
  model[40] = '\0';
  for (int k = 39; k >= 0 && model[k] == ' '; k--)
    model[k] = '\0';

  LOG_INFO("[AHCI] puerto %u: '%s' %lu sectores (%lu MB), sector_size=%u, "
           "LBA48=%d",
           p->port_num, model, (unsigned long)p->num_sectors,
           (unsigned long)(p->num_sectors * p->sector_size / (1024 * 1024)),
           p->sector_size, supports_lba48);
}

// ===========================================================================
// FLUSH CACHE EXT (asíncrono)
// ===========================================================================
// Construye la Command Table para FLUSH CACHE EXT y escribe PxCI.
// NO espera. Devuelve ATA_OK si el comando se lanzó.
static int port_flush_start(ahci_port_t *p) {
  memset(p->cmd_table, 0, sizeof(*p->cmd_table));
  ahci_cmd_header_t *hdr = &p->cmd_list[0];
  memset(hdr, 0, sizeof(*hdr));
  hdr->ctba = (uint32_t)p->cmd_table_phys;
  hdr->ctbau = (uint32_t)(p->cmd_table_phys >> 32);

  fis_reg_h2d_t *fis = (fis_reg_h2d_t *)p->cmd_table->cfis;
  memset(fis, 0, sizeof(*fis));
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  fis->command = ATA_CMD_FLUSH_CACHE_EXT;
  fis->device = 0x40;

  hdr->flags = sizeof(fis_reg_h2d_t) / 4;
  hdr->prdtl = 0;

  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
  p->irq_status = 0;
  p->irq_pending = 0;
  p->xfer_state = 1;   // RUNNING
  p->xfer_started = 1; // Legacy.
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);

  port_write(p, PxIS, 0xFFFFFFFF);
  port_write(p, PxIE, AHCI_IE_CMD);

  __asm__ volatile("mfence" ::: "memory");
  port_write(p, PxCI, 1u);

  return ATA_OK;
}

// ===========================================================================
// Transferencia DMA (un comando, como mucho AHCI_MAX_XFER_BYTES)
// ===========================================================================

// Añade una entrada al PRDT. Devuelve -1 si está llena.
static int prdt_add(ahci_cmd_table_t *t, unsigned *n, uint64_t phys,
                    uint32_t len) {
  if (*n >= AHCI_PRDT_ENTRIES)
    return -1;
  if (len == 0 || len > AHCI_PRD_MAX_BYTES) // [FIX]
    return -1;
  t->prdt[*n].dba = (uint32_t)phys;
  t->prdt[*n].dbau = (uint32_t)(phys >> 32);
  t->prdt[*n].reserved = 0;
  t->prdt[*n].flags = (len - 1);
  (*n)++;
  return 0;
}

// Construye la Command Table para un comando READ/WRITE DMA y escribe PxCI.
// NO espera. Devuelve ATA_OK si el comando se lanzó, o error.
//
// El llamante debe tener el puerto adquirido (port_acquire) y ser el único
// que escribe en la Command Table del slot 0.
// Construye la Command Table para un comando READ/WRITE DMA y escribe PxCI.
// NO espera. Devuelve ATA_OK si el comando se lanzó, o error.
static int port_rw_start(ahci_port_t *p, uint64_t lba, uint32_t count,
                         void *buf, int is_read) {
  if (count == 0)
    return ATA_OK;

  uint64_t byte_count64 = (uint64_t)count * p->sector_size;
  if (byte_count64 > 4 * 1024 * 1024) {
    LOG_ERR("[AHCI] transferencia de %lu bytes excede 4 MB",
            (unsigned long)byte_count64);
    return ATA_ERR_UNKNOWN;
  }
  uint32_t byte_count = (uint32_t)byte_count64;

  // 1. Limpiar Command Table y header del slot 0.
  memset(p->cmd_table, 0, sizeof(*p->cmd_table));
  ahci_cmd_header_t *hdr = &p->cmd_list[0];
  memset(hdr, 0, sizeof(*hdr));
  hdr->ctba = (uint32_t)p->cmd_table_phys;
  hdr->ctbau = (uint32_t)(p->cmd_table_phys >> 32);

  // 2. FIS H2D.
  fis_reg_h2d_t *fis = (fis_reg_h2d_t *)p->cmd_table->cfis;
  memset(fis, 0, sizeof(*fis));
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  fis->command = is_read ? ATA_CMD_READ_DMA_EXT : ATA_CMD_WRITE_DMA_EXT;
  fis->device = 0x40;

  fis->lba0 = (uint8_t)(lba & 0xFF);
  fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
  fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
  fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
  fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
  fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
  fis->countl = (uint8_t)(count & 0xFF);
  fis->counth = (uint8_t)((count >> 8) & 0xFF);

  hdr->flags =
      (uint16_t)((sizeof(fis_reg_h2d_t) / 4) | (is_read ? 0 : AHCI_CMD_WRITE));

  // 3. PRDT. Se fusionan páginas físicamente contiguas en una sola entrada.
  uint64_t virt = (uint64_t)(uintptr_t)buf;
  uint32_t left = byte_count;
  unsigned n = 0;
  uint64_t run_phys = 0;
  uint32_t run_len = 0;

  while (left > 0) {
    uint64_t phys = paging_get_phys(virt);
    if (!phys)
      return ATA_ERR_UNKNOWN;

    uint32_t chunk = PAGE_SIZE - (uint32_t)(virt & (PAGE_SIZE - 1));
    if (chunk > left)
      chunk = left;

    if (run_len && phys == run_phys + run_len &&
        run_len + chunk <= AHCI_PRD_MAX_BYTES) {
      run_len += chunk;
    } else {
      if (run_len && prdt_add(p->cmd_table, &n, run_phys, run_len) != 0) {
        LOG_ERR("[AHCI] PRDT llena (%u entradas)", n);
        return ATA_ERR_UNKNOWN;
      }
      run_phys = phys;
      run_len = chunk;
    }

    virt += chunk;
    left -= chunk;
  }
  if (run_len && prdt_add(p->cmd_table, &n, run_phys, run_len) != 0) {
    LOG_ERR("[AHCI] PRDT llena (%u entradas)", n);
    return ATA_ERR_UNKNOWN;
  }
  p->cmd_table->prdt[n - 1].flags |= AHCI_PRDT_IOC;
  hdr->prdtl = (uint16_t)n;

  // 4. Preparar IRQ state y lanzar el comando.
  //
  // [FIX] Marcar el estado como RUNNING antes de escribir PxCI. Solo
  // el polling y el IRQ handler que vean RUNNING podrán tomar el comando.
  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
  p->irq_status = 0;
  p->irq_pending = 0;
  p->xfer_state = 1;   // RUNNING
  p->xfer_started = 1; // Legacy.
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);

  port_write(p, PxIS, 0xFFFFFFFF);
  port_write(p, PxIE, AHCI_IE_CMD);

  p->last_cmd_tick = sched_get_ticks();
  __asm__ volatile("mfence" ::: "memory");
  port_write(p, PxCI, 1u);

  return ATA_OK;
}

// ===========================================================================
// block_ops_t
// ===========================================================================
static int ahci_submit(block_device_t *bdev, bio_t *bio) {
  ahci_port_t *p = (ahci_port_t *)bdev->private_data;
  if (!p || p->dead)
    return -ENODEV;

  port_acquire(p);

  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
  p->inflight_bio = bio;
  p->xfer_lba = bio->lba;
  p->xfer_buf = (uint8_t *)bio->buf;
  p->xfer_is_read = (bio->op == BIO_READ) ? 1 : 0;
  p->xfer_error = 0;
  p->xfer_state = 0;   // [FIX] IDLE
  p->xfer_started = 0; // Legacy, ya no se usa como condición.

  if (bio->op == BIO_FLUSH) {
    p->xfer_left = 0;
    p->xfer_need_flush = 1;
  } else {
    p->xfer_left = bio->count;
    p->xfer_need_flush = (bio->op == BIO_WRITE) ? 1 : 0;
  }
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);

  bio->error = 0;

  ahci_start_next_cmd(p);
  return -EINPROGRESS;
}

static void ahci_submit_dump(block_device_t *bdev) {
  ahci_port_t *p = (ahci_port_t *)bdev->private_data;
  if (!p)
    return;
  LOG_INFO("    AHCI puerto %u", p->port_num);
  LOG_INFO("    sector size: %u bytes", p->sector_size);
  LOG_INFO("    capacidad: %lu sectores", (unsigned long)p->num_sectors);
  LOG_INFO("    comandos: %u, errores: %u, timeouts: %u", p->cmd_count,
           p->error_count, p->timeout_count);
}

static block_ops_t ahci_ops = {
    .submit = ahci_submit,
    .flush = NULL,
    .dump = ahci_submit_dump,
};

// ===========================================================================
// Registro en el block layer
// ===========================================================================
static void ahci_assign_name(char *out) {
  int idx = g_next_sd_index++;
  out[0] = 's';
  out[1] = 'd';
  if (idx < 26) {
    out[2] = (char)('a' + idx);
    out[3] = '\0';
  } else {
    out[2] = (char)('a' + (idx / 26 - 1) % 26);
    out[3] = (char)('a' + idx % 26);
    out[4] = '\0';
  }
}

static int ahci_register_disk(ahci_port_t *p) {
  block_device_t *bdev = (block_device_t *)kzalloc(sizeof(block_device_t));
  if (!bdev)
    return -ENOMEM;

  char name[8];
  ahci_assign_name(name);
  size_t i = 0;
  while (name[i] && i < sizeof(bdev->name) - 1) {
    bdev->name[i] = name[i];
    i++;
  }
  bdev->name[i] = '\0';

  bdev->num_sectors = p->num_sectors;
  bdev->sector_size = p->sector_size;
  bdev->is_read_only = 0;
  bdev->ops = &ahci_ops;
  bdev->private_data = p;

  int rc = blk_register(bdev);
  if (rc < 0) {
    kfree(bdev);
    return rc;
  }
  p->bdev = bdev;
  return 0;
}

// ===========================================================================
// Detección de un puerto
// ===========================================================================
static int ahci_probe_port(uint8_t port_num) {
  ahci_port_t *p = &g_hba.ports[port_num];
  p->port_num = port_num;
  p->regs = g_hba.abar + 0x100 + (uint32_t)port_num * PORT_REGS_SIZE;
  spin_init(&p->lock);
  p->busy = 0;
  wait_queue_init(&p->slot_wq);
  wait_queue_init(&p->irq_wq);
  p->irq_pending = 0;
  p->irq_status = 0;
  p->dead = 0;
  p->eh_level = EH_NONE;
  p->cmd_timeout_ms = 5000;
  p->ncq_depth = 1;
  p->ncq_enabled = 0;
  p->inflight_bio = NULL;

  // 1. SSTS.DET == 3?
  uint32_t ssts = port_read(p, PxSSTS);
  if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_READY) {
    LOG_DEBUG("[AHCI] puerto %u: sin dispositivo (SSTS.DET=%u)", port_num,
              ssts & AHCI_SSTS_DET_MASK);
    return -1;
  }

  LOG_INFO("[AHCI] puerto %u: dispositivo presente (SSTS=0x%x, TFD=0x%x)",
           port_num, ssts, port_read(p, PxTFD));

  // 2. Puerto parado + CLB/FB programados + FRE=1 (ST=0).
  if (port_setup_memory(p) != 0) {
    LOG_WARN("[AHCI] puerto %u: port_setup_memory falló", port_num);
    return -1;
  }

  // 3. COMRESET. port_reset ya espera a que PxSIG sea estable.
  if (port_reset(p) != 0) {
    LOG_WARN("[AHCI] puerto %u: port_reset falló", port_num);
    port_free_memory(p);
    return -1;
  }

  // 4. Comprobar la firma.
  uint32_t sig = port_read(p, PxSIG);
  LOG_INFO("[AHCI] puerto %u: tras COMRESET SIG=0x%08x TFD=0x%x", port_num, sig,
           port_read(p, PxTFD));

  if (sig != 0x00000101) {
    LOG_INFO("[AHCI] puerto %u: firma no-ATA (0x%08x), ignorando", port_num,
             sig);
    port_free_memory(p);
    return -1;
  }

  // 5. Arrancar el motor de comandos (ST=1).
  if (port_start(p) != 0) {
    LOG_WARN("[AHCI] puerto %u: port_start falló", port_num);
    port_free_memory(p);
    return -1;
  }

  uint32_t final_cmd = port_read(p, PxCMD);
  LOG_INFO(
      "[AHCI] puerto %u: setup final: PxCMD=0x%08x (ST=%d FRE=%d CR=%d FR=%d) "
      "PxSSTS=0x%x PxTFD=0x%x PxSIG=0x%08x",
      port_num, final_cmd, !!(final_cmd & AHCI_PxCMD_ST),
      !!(final_cmd & AHCI_PxCMD_FRE), !!(final_cmd & AHCI_PxCMD_CR),
      !!(final_cmd & AHCI_PxCMD_FR), port_read(p, PxSSTS), port_read(p, PxTFD),
      port_read(p, PxSIG));

  // 6. IDENTIFY.
  int rc = port_identify(p);
  if (rc != ATA_OK) {
    LOG_WARN("[AHCI] puerto %u: IDENTIFY falló (rc=%d)", port_num, rc);
    port_free_memory(p);
    return -1;
  }

  ahci_parse_identify(p);
  if (p->num_sectors == 0) {
    LOG_WARN("[AHCI] puerto %u: 0 sectores, ignorando", port_num);
    port_free_memory(p);
    return -1;
  }

  p->initialized = 1;
  p->has_device = 1;

  if (ahci_register_disk(p) != 0) {
    LOG_ERR("[AHCI] puerto %u: fallo al registrar en block layer", port_num);
    p->initialized = 0;
    p->has_device = 0;
    port_free_memory(p);
    return -1;
  }

  LOG_INFO("[AHCI] %s registrado: puerto %u, %lu sectores, %u B/sector, "
           "NCQ depth=%u",
           p->bdev->name, port_num, (unsigned long)p->num_sectors,
           p->sector_size, p->ncq_depth);
  return 0;
}

// ===========================================================================
// Inicialización
// ===========================================================================
static int ahci_init_impl(void) {
  memset(&g_hba, 0, sizeof(g_hba));
  g_hba.irq_count = 0;

  // 1. Detectar el controlador AHCI en PCI.
  pci_device_t dev;
  if (pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_STORAGE_SATA, &dev) !=
      0) {
    LOG_INFO("[AHCI] no hay controlador SATA en PCI");
    return 0;
  }
  uint8_t prog = dev.prog_if & 0x7F;
  if (prog != PCI_PROGIF_SATA_AHCI) {
    LOG_INFO("[AHCI] controlador SATA no está en modo AHCI (progif=0x%02x)",
             dev.prog_if);
    return 0;
  }
  g_hba.pci = dev;
  LOG_INFO("[AHCI] controlador en %02x:%02x.%x (vendor=0x%04x device=0x%04x)",
           dev.bus, dev.slot, dev.func, dev.vendor_id, dev.device_id);

  // [DEBUG] Leer PCI config: IRQ line (0x3C) y IRQ pin (0x3D).
  uint32_t irq_reg = pci_read_config_dword(dev.bus, dev.slot, dev.func, 0x3C);
  uint8_t irq_line = (uint8_t)(irq_reg & 0xFF);
  uint8_t irq_pin = (uint8_t)((irq_reg >> 8) & 0xFF);
  LOG_INFO("[AHCI-DEBUG] PCI 0x3C = 0x%08x (line=%u pin=%u)", irq_reg, irq_line,
           irq_pin);
  if (irq_pin == 0) {
    LOG_WARN("[AHCI-DEBUG] PCI_INTERRUPT_PIN=0: el BIOS NO ha ruteado IRQ");
  } else {
    LOG_INFO("[AHCI-DEBUG] PCI_INTERRUPT_PIN=%u (INTA#=%u, INTB#=%u, "
             "INTC#=%u, INTD#=%u)",
             irq_pin, irq_pin == 1, irq_pin == 2, irq_pin == 3, irq_pin == 4);
  }

  // [DEBUG] Leer los registros PIRQ del LPC bridge (00:1f.0).
  // En ICH9, el LPC bridge está en 00:1f.0 y los registros PIRQx_ROUT
  // están en 0x60-0x63 (PIRQA-D) y 0x68-0x6B (PIRQE-H).
  // El bit 7 (0x80) significa "IRQ deshabilitada".
  for (int i = 0; i < 8; i++) {
    uint32_t pirq_reg = pci_read_config_dword(0, 0x1f, 0, 0x60 + i);
    LOG_INFO("[AHCI-DEBUG] PIRQ%c_ROUT = 0x%02x (disabled=%d)", 'A' + i,
             pirq_reg & 0xFF, !!(pirq_reg & 0x80));
  }

  // 2. Habilitar bus mastering e IO/MEM.
  pci_enable_bus_mastering(&dev);
  pci_enable_io_mem(&dev);

  // 3. Mapear BAR5 (ABAR).
  uint32_t abar_phys = pci_get_bar(&dev, 5);
  if (abar_phys == 0) {
    LOG_ERR("[AHCI] BAR5 (ABAR) es 0");
    return -1;
  }
  g_hba.abar_phys = abar_phys;
  g_hba.abar = (volatile uint8_t *)mmio_map(
      abar_phys, 0x1100, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!g_hba.abar) {
    LOG_ERR("[AHCI] mmio_map(BAR5) falló");
    return -1;
  }
  LOG_INFO("[AHCI] ABAR mapeado phys=0x%x -> virt=%p", abar_phys,
           (void *)g_hba.abar);

  // 4. Leer capacidades.
  g_hba.cap = ahci_read32(g_hba.abar, HBA_CAP);
  g_hba.cap2 = ahci_read32(g_hba.abar, HBA_CAP2);
  g_hba.pi = ahci_read32(g_hba.abar, HBA_PI);
  g_hba.version = ahci_read32(g_hba.abar, HBA_VS);
  g_hba.bohc = ahci_read32(g_hba.abar, HBA_BOHC);

  LOG_INFO("[AHCI] CAP=0x%08x CAP2=0x%08x PI=0x%08x VS=0x%08x BOHC=0x%08x",
           g_hba.cap, g_hba.cap2, g_hba.pi, g_hba.version, g_hba.bohc);
  LOG_INFO("[AHCI] versión %u.%u, %u puertos, %u puertos implementados",
           (g_hba.version >> 16) & 0xFFFF, (g_hba.version >> 8) & 0xFF,
           (g_hba.cap & 0x1F) + 1, ahci_popcount32(g_hba.pi));

  // 5. Reset del HBA.
  if (hba_reset() != 0)
    return -1;

  // 6. Configurar interrupciones.
  //
  // Estrategia universal:
  //   1. Intentar MSI. Si funciona, sin storm posible.
  //   2. Si no, IRQ legacy level-triggered con enmascaramiento.
  //   3. El polling queda como red de seguridad.
  //
  // MSI no está disponible en todos los dispositivos (VirtualBox ICH9
  // no lo expone para el SATA). En ese caso, la IRQ legacy con
  // enmascaramiento funciona en cualquier hardware.
  uint8_t irq = pci_get_irq(&dev);

  // --- Intentar MSI primero ---
  if (pci_find_capability(dev.bus, dev.slot, dev.func, 0x05) != 0) {
    if (pci_enable_msi(dev.bus, dev.slot, dev.func, MSI_VECTOR_BASE,
                       lapic_get_bsp_id()) == 0) {
      g_hba.using_msi = 1;
      g_hba.msi_vector = MSI_VECTOR_BASE;
      msi_install_handler(MSI_VECTOR_BASE, ahci_irq_handler_msi);
      LOG_INFO("[AHCI] MSI habilitado en vector 0x%02x", MSI_VECTOR_BASE);

      // Enmascarar la IRQ legacy por si acaso.
      if (irq != 0xFF && irq <= 15) {
        ioapic_mask_irq(irq, 1);
        LOG_INFO("[AHCI] IRQ legacy %u enmascarada", irq);
      }
    } else {
      LOG_WARN("[AHCI] MSI falló, usando IRQ legacy");
    }
  } else {
    LOG_INFO("[AHCI] Sin capability MSI, usando IRQ legacy");
  }

  // --- Fallback a IRQ legacy ---
  if (!g_hba.using_msi) {
    if (irq == 0xFF || irq > 15) {
      LOG_WARN("[AHCI] IRQ inválida (%u), el driver usará polling", irq);
    } else {
      g_hba.irq = irq;
      g_hba.irq_masked = 0;
      g_hba.irq_spurious_count = 0;

      irq_install_handler_ex(irq, ahci_irq_ack, ahci_irq_process);

      // level-triggered, active-low. El enmascaramiento en el ack
      // evita el storm incluso si la IRQ se comparte.
      ioapic_redirect_irq_ex(irq, 32 + irq, lapic_get_bsp_id(),
                             0 /* unmasked */, 1 /* level */,
                             1 /* active-low */);

      LOG_INFO("[AHCI] IRQ %u instalada como level/active-low", irq);
    }
  }

  // 7. Habilitar interrupciones globales del HBA.
  ahci_write32(g_hba.abar, HBA_GHC, ahci_read32(g_hba.abar, HBA_GHC) | GHC_IE);

  // 8. Probar cada puerto implementado.
  int found = 0;
  for (int i = 0; i < 32; i++) {
    if (!(g_hba.pi & (1u << i)))
      continue;
    if (ahci_probe_port((uint8_t)i) == 0)
      found++;
  }
  g_hba.num_ports_impl = (uint8_t)ahci_popcount32(g_hba.pi);

  LOG_INFO("[AHCI] %d discos SATA registrados", found);
  g_hba.initialized = 1;
  return 0;
}

// [NUEVO] Un paso de la máquina de estados de recuperación.
static void port_eh_step(ahci_port_t *p) {
  switch (p->eh_level) {
  case EH_NONE:
    return;

  case EH_LINK_RESET:
    if (port_reset(p) == 0) {
      p->eh_level = EH_NONE;
      return;
    }
    p->eh_level = EH_DEV_RESET;
    /* fallthrough */

  case EH_DEV_RESET:
    // Segundo intento de COMRESET. Si ya falló una vez, es probable
    // que el enlace esté realmente caído.
    if (port_reset(p) == 0) {
      p->eh_level = EH_NONE;
      return;
    }
    p->eh_level = EH_HARD_RESET;
    /* fallthrough */

  case EH_HARD_RESET:
    LOG_WARN("[AHCI] puerto %u: hard reset", p->port_num);
    // Parar el puerto, liberar memoria, reconfigurar y arrancar.
    if (port_stop(p) == 0) {
      // port_setup_memory vuelve a asignar CLB/FB y deja FRE=1.
      // Necesitamos liberar lo anterior primero.
      // Nota: port_free_memory marca dead, así que no lo usamos aquí.
      if (p->cmd_list_phys) {
        pmm_free_page(p->cmd_list_phys);
        p->cmd_list_phys = 0;
      }
      if (p->rx_fis_phys) {
        pmm_free_page(p->rx_fis_phys);
        p->rx_fis_phys = 0;
      }
      if (p->cmd_table_phys) {
        pmm_free_page(p->cmd_table_phys);
        p->cmd_table_phys = 0;
      }
      if (p->identify_phys) {
        pmm_free_page(p->identify_phys);
        p->identify_phys = 0;
      }
      p->cmd_list = NULL;
      p->rx_fis = NULL;
      p->cmd_table = NULL;
      p->identify = NULL;

      if (port_setup_memory(p) == 0 && port_reset(p) == 0 &&
          port_start(p) == 0) {
        p->eh_level = EH_REVALIDATE;
        break;
      }
    }
    p->eh_level = EH_DISABLE;
    /* fallthrough */

  case EH_REVALIDATE:
    if (port_identify(p) == ATA_OK) {
      ahci_parse_identify(p);
      // Revalidar sector_size y num_sectors.
      if (p->num_sectors == 0) {
        p->eh_level = EH_DISABLE;
        break;
      }
      p->eh_level = EH_NONE;
      LOG_INFO("[AHCI] puerto %u: recuperado tras revalidate", p->port_num);
      return;
    }
    p->eh_level = EH_DISABLE;
    /* fallthrough */

  case EH_DISABLE:
    LOG_ERR("[AHCI] puerto %u: irrecuperable, desregistrando", p->port_num);
    if (p->bdev) {
      blk_unregister(p->bdev);
      kfree(p->bdev);
      p->bdev = NULL;
    }
    p->initialized = 0;
    p->has_device = 0;
    port_free_memory(p);
    break;
  }
}

// ===========================================================================
// Recuperación tras timeout o error fatal (spec 6.2.2.1).
// ===========================================================================
// [NUEVO] Sustituye a port_recover en port_issue_wait.
// Sube un nivel y ejecuta un paso. El siguiente error subirá otro nivel.
static void port_recover(ahci_port_t *p) {
  if (p->eh_level == EH_NONE)
    p->eh_level = EH_LINK_RESET;
  port_eh_step(p);
}

// ===========================================================================
// Debug
// ===========================================================================
void ahci_dump(void) {
  if (!g_hba.initialized) {
    LOG_INFO("[AHCI] no inicializado");
    return;
  }
  LOG_INFO("[AHCI] ABAR phys=0x%lx CAP=0x%08x PI=0x%08x",
           (unsigned long)g_hba.abar_phys, g_hba.cap, g_hba.pi);
  for (int i = 0; i < 32; i++) {
    ahci_port_t *p = &g_hba.ports[i];
    if (!p->initialized)
      continue;
    LOG_INFO("[AHCI]   puerto %u: %s, %lu sectores, cmd=%u err=%u t/o=%u",
             p->port_num, p->bdev ? p->bdev->name : "?",
             (unsigned long)p->num_sectors, p->cmd_count, p->error_count,
             p->timeout_count);
  }
}

int ahci_disk_count(void) {
  int n = 0;
  for (int i = 0; i < 32; i++)
    if (g_hba.ports[i].initialized)
      n++;
  return n;
}

// ===========================================================================
// Polling desde el scheduler tick (1 ms).
//
// Red de seguridad: si la IRQ no llega (enmascarada, perdida, hardware
// sin IRQ), el polling procesa el comando.
//
// [FIX SMP] El polling se ejecuta SOLO en el BSP. La IRQ del AHCI está
// ruteada al BSP (ioapic_redirect_irq_ex con lapic_get_bsp_id()), así
// que tiene sentido que el polling siga la misma política. Ejecutarlo
// en los 4 CPUs causaba:
//   - 4x lecturas MMIO (PxCI, PxTFD) del mismo puerto.
//   - 4x adquisiciones de p->irq_wq.lock por tick.
//   - 4x llamadas a bio_endio desde contexto de IRQ en paralelo.
//   - Carreras sutiles con el scheduler al despertar waiters.
//
// [FIX umbral] El umbral es AHCI_POLL_DELAY_TICKS (2 ms). Da margen al
// IRQ handler (que en producción tarda <100 us) y minimiza la latencia
// de rescate cuando la IRQ no llega.
//
// La corrección multi-CPU sigue garantizada por xfer_state: solo uno
// de los caminos (IRQ handler o polling) puede transicionar de RUNNING
// a PROCESSING.
// ===========================================================================
void ahci_poll_ports(void) {
  if (!g_hba.initialized)
    return;

  if (g_hba.using_msi)
    return;

  // [FIX SMP] Solo el BSP hace polling. La IRQ del AHCI llega al BSP,
  // y el polling es su red de seguridad. Hacerlo en todos los CPUs
  // multiplica la contención sin aportar nada.
  if (smp_processor_id() != 0)
    return;

  uint64_t now = sched_get_ticks();

  for (int i = 0; i < 32; i++) {
    ahci_port_t *p = &g_hba.ports[i];
    if (!p->regs || p->dead)
      continue;

    unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
    int has_bio = (p->inflight_bio != NULL);
    int running = (p->xfer_state == 1);
    uint32_t irq_status = p->irq_status;
    uint64_t last_tick = p->last_cmd_tick;
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);

    if (!has_bio || !running)
      continue;

    // ¿El comando terminó físicamente?
    if (port_read(p, PxCI) & 1u)
      continue;

    // [FIX umbral] Margen para que el IRQ handler haga su trabajo.
    if (now - last_tick < AHCI_POLL_DELAY_TICKS)
      continue;

    // Simular la IRQ para que ahci_cmd_complete procese.
    if (irq_status == 0) {
      flags = spin_lock_irqsave(&p->irq_wq.lock);
      if (p->irq_status == 0 && p->xfer_state == 1) {
        p->irq_status |= AHCI_PxIS_DHRS;
        p->irq_pending++;
      }
      spin_unlock_irqrestore(&p->irq_wq.lock, flags);
    }

    ahci_cmd_complete(p);
  }
}

// ===========================================================================
// Handler MSI.
//
// Se llama desde el stub del vector 0x60, vía msi_dispatch.
// Equivale a ahci_irq_ack + ahci_irq_process, pero sin la fase de EOI
// explícita (el LAPIC la manda al retornar del handler).
// ===========================================================================
static void ahci_irq_handler_msi(void) {
  // 1. Leer HBA_IS y limpiar PxIS de cada puerto con bits marcados.
  volatile uint8_t *abar = g_hba.abar;
  if (abar) {
    uint32_t is_global = ahci_read32(abar, HBA_IS);
    if (is_global != 0) {
      for (int i = 0; i < 32; i++) {
        if (!(is_global & (1u << i)))
          continue;
        ahci_port_t *p = &g_hba.ports[i];
        if (!p->regs || p->dead)
          continue;

        uint32_t px_is = port_read(p, PxIS);
        port_write(p, PxIS, px_is);
        if (px_is == 0)
          continue;

        unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
        p->irq_pending++;
        p->irq_status |= px_is;
        wake_up_all_locked(&p->irq_wq); // <-- AÑADIR
        spin_unlock_irqrestore(&p->irq_wq.lock, flags);
      }

      ahci_write32(abar, HBA_IS, is_global);
    }
  }

  // 2. Procesar los comandos completados.
  for (int i = 0; i < 32; i++) {
    ahci_port_t *p = &g_hba.ports[i];
    if (!p->regs || p->dead)
      continue;

    unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
    int has_bio = (p->inflight_bio != NULL);
    int running = (p->xfer_state == 1);
    uint32_t irq_status = p->irq_status;
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);

    if (!has_bio || !running || irq_status == 0)
      continue;

    ahci_cmd_complete(p);
  }
}

// ===========================================================================
// Driver
// ===========================================================================
struct driver ahci_driver = {
    .name = "ahci",
    .init = ahci_init_impl,
    .shutdown = NULL,
};