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

// ===========================================================================
// Helpers de puerto
// ===========================================================================
static void port_write(ahci_port_t *p, uint32_t off, uint32_t val) {
  ahci_write32(p->regs, off, val);
}
static uint32_t port_read(ahci_port_t *p, uint32_t off) {
  return ahci_read32(p->regs, off);
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
    ahci_delay_ms(1);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Condiciones para las wait queues.
//
// ahci_cmd_done: el comando del slot 0 terminó (PxCI bit 0 = 0) o el HBA
// notificó un error fatal. Se llama con irq_wq.lock cogido; solo lee MMIO.
//
// ahci_slot_free: el slot 0 está libre.
// ---------------------------------------------------------------------------
static bool ahci_cmd_done(void *arg) {
  ahci_port_t *p = (ahci_port_t *)arg;
  if (!(port_read(p, PxCI) & 1u))
    return true;
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

// Para el motor de comandos (ST=0) y espera CR=0. FRE no se toca.
static int port_stop_cmd(ahci_port_t *p) {
  uint32_t cmd = port_read(p, PxCMD);
  port_write(p, PxCMD, cmd & ~AHCI_PxCMD_ST);
  for (int i = 0; i < 500; i++) {
    if (!(port_read(p, PxCMD) & AHCI_PxCMD_CR))
      return 0;
    ahci_delay_ms(1);
  }
  LOG_WARN("[AHCI] puerto %u: CR no se limpió tras ST=0", p->port_num);
  return -1;
}

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

  LOG_DEBUG("[AHCI] puerto %u: reset completado, SSTS=0x%x, TFD=0x%x",
            p->port_num, port_read(p, PxSSTS), port_read(p, PxTFD));
  return 0;
}

// ===========================================================================
// Recuperación tras timeout o error fatal (spec 6.2.2.1).
// ===========================================================================
static void port_recover(ahci_port_t *p) {
  port_stop_cmd(p);
  port_write(p, PxSERR, 0xFFFFFFFF);
  port_write(p, PxIS, 0xFFFFFFFF);

  if (port_read(p, PxTFD) & (TFD_BSY | TFD_DRQ)) {
    if (g_hba.cap & CAP_SCLO) {
      port_write(p, PxCMD, port_read(p, PxCMD) | AHCI_PxCMD_CLO);
      for (int i = 0; i < 500; i++) {
        if (!(port_read(p, PxCMD) & AHCI_PxCMD_CLO))
          break;
        ahci_delay_ms(1);
      }
    } else {
      port_reset(p);
    }
  }

  if (port_start(p) != 0)
    LOG_ERR("[AHCI] puerto %u: no se pudo recuperar el puerto", p->port_num);
}

// ===========================================================================
// Liberar memoria del puerto
// ===========================================================================
static void port_free_memory(ahci_port_t *p) {
  // El HBA no debe seguir usando estas páginas: parar el puerto primero.
  if (p->regs) {
    if (port_stop(p) != 0) {
      LOG_ERR("[AHCI] puerto %u: no se pudo parar; se filtran las páginas "
              "para no corromper memoria por DMA",
              p->port_num);
      return;
    }
    port_write(p, PxIE, 0);
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
  if (!p->cmd_list_phys || (p->cmd_list_phys & 0x3FF)) {
    LOG_ERR("[AHCI] puerto %u: no se pudo asignar Command List alineada",
            p->port_num);
    port_free_memory(p);
    return -1;
  }
  p->cmd_list = (ahci_cmd_header_t *)phys_to_virt(p->cmd_list_phys);
  memset(p->cmd_list, 0, PAGE_SIZE);

  // --- RX FIS: 256 B, alineada a 256 B (página). ---
  p->rx_fis_phys = pmm_alloc_page();
  if (!p->rx_fis_phys || (p->rx_fis_phys & 0xFF)) {
    LOG_ERR("[AHCI] puerto %u: no se pudo asignar RX FIS", p->port_num);
    port_free_memory(p);
    return -1;
  }
  p->rx_fis = (ahci_rx_fis_t *)phys_to_virt(p->rx_fis_phys);
  memset(p->rx_fis, 0, PAGE_SIZE);

  // --- Command Table: alineada a 128 B (página). ---
  p->cmd_table_phys = pmm_alloc_page();
  if (!p->cmd_table_phys || (p->cmd_table_phys & 0x7F)) {
    LOG_ERR("[AHCI] puerto %u: no se pudo asignar Command Table", p->port_num);
    port_free_memory(p);
    return -1;
  }
  p->cmd_table = (ahci_cmd_table_t *)phys_to_virt(p->cmd_table_phys);
  memset(p->cmd_table, 0, PAGE_SIZE);

  // --- Buffer IDENTIFY: 1 página propia, 512 B usados. ---
  p->identify_phys = pmm_alloc_page();
  if (!p->identify_phys || (p->identify_phys & 0xFFF)) {
    LOG_ERR("[AHCI] puerto %u: no se pudo asignar buffer IDENTIFY",
            p->port_num);
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
  unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
  p->irq_status = 0;
  spin_unlock_irqrestore(&p->irq_wq.lock, flags);

  port_write(p, PxIS, 0xFFFFFFFF);
  port_write(p, PxIE, ie_mask);

  // La Command Table y el header deben ser visibles antes de PxCI.
  __asm__ volatile("mfence" ::: "memory");
  port_write(p, PxCI, 1u); // escribir 0 en el resto de bits no tiene efecto

  // Un DMA en vuelo no se puede abandonar (escribiría en memoria del
  // llamante): si nos interrumpen (-EINTR) con el comando aún activo,
  // volvemos a esperar.
  long w = 0;
  for (int tries = 0; tries < 4; tries++) {
    w = wait_event_interruptible_timeout(&p->irq_wq, ahci_cmd_done, p, 5000);
    if (w >= 0)
      break;
    if (ahci_cmd_done(p))
      break;
  }

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

  p->cmd_count++;
  return ATA_OK;
}

// ===========================================================================
// IDENTIFY DEVICE
// ===========================================================================
static int port_identify(ahci_port_t *p) {
  // 1. Limpiar Command Table y header.
  memset(p->cmd_table, 0, sizeof(*p->cmd_table));
  ahci_cmd_header_t *hdr = &p->cmd_list[0];
  memset(hdr, 0, sizeof(*hdr));
  hdr->ctba = (uint32_t)p->cmd_table_phys;
  hdr->ctbau = (uint32_t)(p->cmd_table_phys >> 32);

  // 2. FIS H2D para IDENTIFY DEVICE.
  fis_reg_h2d_t *fis = (fis_reg_h2d_t *)p->cmd_table->cfis;
  memset(fis, 0, sizeof(*fis));
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  fis->command = ATA_CMD_IDENTIFY;

  // 3. PRDT: 512 B al buffer IDENTIFY (página propia => una entrada).
  p->cmd_table->prdt[0].dba = (uint32_t)p->identify_phys;
  p->cmd_table->prdt[0].dbau = (uint32_t)(p->identify_phys >> 32);
  p->cmd_table->prdt[0].reserved = 0;
  p->cmd_table->prdt[0].flags = (512 - 1) | AHCI_PRDT_IOC;

  hdr->flags = sizeof(fis_reg_h2d_t) / 4; // CFL; W=0 (device -> host)
  hdr->prdtl = 1;

  memset(p->identify, 0, 512);

  // 4. Enviar y esperar (un solo envío de PxCI).
  LOG_DEBUG("[AHCI] puerto %u: enviando IDENTIFY", p->port_num);
  int rc = port_issue_wait(p, AHCI_IE_CMD);
  if (rc != ATA_OK)
    return rc;

  LOG_DEBUG("[AHCI] puerto %u: IDENTIFY terminó, PxCI=0x%x PxTFD=0x%x "
            "PRDBC=%u",
            p->port_num, port_read(p, PxCI), port_read(p, PxTFD), hdr->prdbc);
  LOG_DEBUG("[AHCI] puerto %u: identify[0..7] = %04x %04x %04x %04x %04x %04x "
            "%04x %04x",
            p->port_num, p->identify[0], p->identify[1], p->identify[2],
            p->identify[3], p->identify[4], p->identify[5], p->identify[6],
            p->identify[7]);

  // 5. Sanity check del buffer.
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
// FLUSH CACHE EXT
// ===========================================================================
static int port_flush(ahci_port_t *p) {
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

  return port_issue_wait(p, AHCI_IE_CMD);
}

// ===========================================================================
// Transferencia DMA (un comando, como mucho AHCI_MAX_XFER_BYTES)
// ===========================================================================

// Añade una entrada al PRDT. Devuelve -1 si está llena.
static int prdt_add(ahci_cmd_table_t *t, unsigned *n, uint64_t phys,
                    uint32_t len) {
  if (*n >= AHCI_PRDT_ENTRIES)
    return -1;
  t->prdt[*n].dba = (uint32_t)phys;
  t->prdt[*n].dbau = (uint32_t)(phys >> 32);
  t->prdt[*n].reserved = 0;
  t->prdt[*n].flags = (len - 1);
  (*n)++;
  return 0;
}

static int port_rw(ahci_port_t *p, uint64_t lba, uint32_t count, void *buf,
                   int is_read) {
  if (count == 0)
    return ATA_OK;

  uint32_t byte_count = count * p->sector_size;
  if (byte_count > 4 * 1024 * 1024) {
    LOG_ERR("[AHCI] transferencia de %u bytes excede 4 MB", byte_count);
    return ATA_ERR_UNKNOWN;
  }

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

  // CFL + bit W en las escrituras (H2D). Los HBA reales lo usan para la
  // dirección del DMA; QEMU lo ignora.
  hdr->flags =
      (uint16_t)((sizeof(fis_reg_h2d_t) / 4) | (is_read ? 0 : AHCI_CMD_WRITE));

  // 3. PRDT. Se fusionan páginas físicamente contiguas en una sola entrada
  //    (hasta 4 MB por entrada).
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

  // 4. Enviar y esperar.
  int rc = port_issue_wait(p, AHCI_IE_CMD);
  if (rc != ATA_OK) {
    LOG_ERR("[AHCI] puerto %u: fallo en %s lba=%lu count=%u (rc=%d)",
            p->port_num, is_read ? "READ" : "WRITE", (unsigned long)lba, count,
            rc);
  }
  return rc;
}

// Ejecuta un bio de lectura/escritura troceándolo en comandos de como mucho
// AHCI_MAX_XFER_BYTES. Tras una escritura hace FLUSH CACHE EXT una vez.
static int port_xfer(ahci_port_t *p, bio_t *bio, int is_read) {
  uint32_t max_sectors = AHCI_MAX_XFER_BYTES / p->sector_size;
  if (max_sectors == 0)
    max_sectors = 1;

  uint8_t *b = (uint8_t *)bio->buf;
  uint64_t lba = bio->lba;
  uint32_t left = bio->count;
  int rc = ATA_OK;

  while (left > 0 && rc == ATA_OK) {
    uint32_t n = (left < max_sectors) ? left : max_sectors;
    rc = port_rw(p, lba, n, b, is_read);
    b += (uint64_t)n * p->sector_size;
    lba += n;
    left -= n;
  }

  if (rc == ATA_OK && !is_read)
    rc = port_flush(p);
  return rc;
}

// ===========================================================================
// block_ops_t
// ===========================================================================
static int ahci_submit(block_device_t *bdev, bio_t *bio) {
  ahci_port_t *p = (ahci_port_t *)bdev->private_data;
  if (!p)
    return -ENODEV;

  // Serializa el slot 0 SIN spinlock durante la espera (se duerme dentro).
  port_acquire(p);

  int rc;
  switch (bio->op) {
  case BIO_READ:
    rc = port_xfer(p, bio, 1);
    break;
  case BIO_WRITE:
    rc = port_xfer(p, bio, 0);
    break;
  case BIO_FLUSH:
    rc = port_flush(p);
    break;
  default:
    port_release(p);
    bio->error = -EINVAL;
    return bio->error;
  }

  port_release(p);

  bio->error = (rc == ATA_OK) ? 0 : -EIO;
  return bio->error;
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
// IRQ handler
//
// Orden exigido por la spec: primero PxIS de cada puerto, DESPUÉS IS global.
// Si se limpia IS con PxIS aún activo, el bit se vuelve a poner.
// ===========================================================================
static void ahci_irq_handler(void) {
  volatile uint8_t *abar = g_hba.abar;
  if (!abar)
    return;

  uint32_t is_global = ahci_read32(abar, HBA_IS);
  if (is_global == 0)
    return;

  for (int i = 0; i < 32; i++) {
    if (!(is_global & (1u << i)))
      continue;
    ahci_port_t *p = &g_hba.ports[i];
    // Solo procesar puertos ya configurados (con wait queue inicializada).
    if (!p->regs)
      continue;

    uint32_t px_is = port_read(p, PxIS);
    port_write(p, PxIS, px_is);
    if (px_is == 0)
      continue;

    unsigned long flags = spin_lock_irqsave(&p->irq_wq.lock);
    p->irq_pending++;
    p->irq_status |= px_is; // acumular: pueden llegar varias IRQ por comando
    wake_up_all_locked(&p->irq_wq);
    spin_unlock_irqrestore(&p->irq_wq.lock, flags);
  }

  ahci_write32(abar, HBA_IS, is_global);
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

  // 3. COMRESET (con FRE=1 el HBA puede volcar el FIS de firma).
  if (port_reset(p) != 0) {
    LOG_WARN("[AHCI] puerto %u: port_reset falló", port_num);
    port_free_memory(p);
    return -1;
  }

  // 4. Esperar a que la firma sea válida y comprobarla.
  uint32_t sig = port_read(p, PxSIG);
  for (int i = 0; i < 500 && (sig == 0x00000000 || sig == 0xFFFFFFFF); i++) {
    ahci_delay_ms(1);
    sig = port_read(p, PxSIG);
  }
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

  LOG_INFO("[AHCI] %s registrado: puerto %u, %lu sectores, %u B/sector",
           p->bdev->name, port_num, (unsigned long)p->num_sectors,
           p->sector_size);
  return 0;
}

// ===========================================================================
// Inicialización
// ===========================================================================
static int ahci_init_impl(void) {
  memset(&g_hba, 0, sizeof(g_hba));

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

  // 6. Instalar el IRQ handler.
  uint8_t irq = pci_get_irq(&dev);
  if (irq == 0xFF || irq > 15) {
    LOG_WARN("[AHCI] IRQ inválida (%u), el driver no funcionará", irq);
  } else {
    irq_install_handler(irq, ahci_irq_handler);
    LOG_INFO("[AHCI] IRQ %u instalada", irq);
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
// Driver
// ===========================================================================
struct driver ahci_driver = {
    .name = "ahci",
    .init = ahci_init_impl,
    .shutdown = NULL,
};