// kernel/ata_dma.c
//
// Soporte de DMA para ATA (Bus Master IDE).
//
// Referencia: Intel PIIX3 datasheet, ATA/ATAPI-7.

#include "ata_dma.h"
#include "ata_common.h"
#include "ata_device.h"
#include "ata_pio.h"
#include "block.h"
#include "cpu.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "wait.h"

extern uint64_t paging_get_phys(uint64_t virt);

// ---------------------------------------------------------------------------
// Logs de diagnóstico.
// Descomenta la línea de abajo para ver logs detallados de DMA (uno por
// transferencia; desactivado por defecto).
// ---------------------------------------------------------------------------
// #define ATA_DMA_DEBUG

#ifdef ATA_DMA_DEBUG
#define DMA_LOG(...) LOG_INFO(__VA_ARGS__)
#else
#define DMA_LOG(...)                                                           \
  do {                                                                         \
  } while (0)
#endif

// ===========================================================================
// Estado global
// ===========================================================================
static ata_dma_state_t g_dma[2];

// Registros del BMIDE (offsets desde bmide_base).
#define BM_CMD 0x00
#define BM_STATUS 0x02
#define BM_PRDT 0x04

// Entradas maximas de la PRDT (una pagina de 4 KB, 8 bytes por entrada).
#define ATA_DMA_MAX_PRD (PAGE_SIZE / sizeof(ata_dma_prd_t))

// Delay tras programar el BMIDE.
static void ata_dma_delay(void) {
  for (volatile int i = 0; i < 100; i++) {
    __asm__ volatile("pause");
  }
}

// ===========================================================================
// Inicialización
// ===========================================================================
int ata_dma_init_channel(int channel_idx, uint8_t pci_bus, uint8_t pci_slot,
                         uint8_t pci_func) {
  if (channel_idx < 0 || channel_idx > 1)
    return -1;

  ata_dma_state_t *dma = &g_dma[channel_idx];
  memset(dma, 0, sizeof(*dma));

  // 1. Leer BAR4 del PCI IDE.
  extern uint32_t pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func,
                               int bar_num);
  uint32_t bar4 = pci_read_bar(pci_bus, pci_slot, pci_func, 4);
  if (bar4 == 0) {
    LOG_ERR("[ATA-DMA] BAR4 del IDE es 0");
    return -1;
  }

  dma->bmide_base = (uint16_t)(bar4 + channel_idx * 8);
  dma->irq = (channel_idx == 0) ? ATA_PRIMARY_IRQ : ATA_SECONDARY_IRQ;
  dma->channel_idx = (uint8_t)channel_idx;

  LOG_INFO("[ATA-DMA] BMIDE canal %s: base=0x%x, IRQ=%u",
           channel_idx == 0 ? "primario" : "secundario", dma->bmide_base,
           dma->irq);

  // 2. Asignar PRDT.
  //
  // Pagina fisica propia (PMM) accedida por la ventana fisica: la direccion
  // fisica es exacta, la tabla esta alineada a 4 KB (no cruza 64 KB) y es
  // contigua. NO usar kmalloc()+virt_to_phys(): el heap vive en
  // 0xffffffff82xxxxxx (mapeado pagina a pagina) y virt_to_phys() solo vale
  // para la ventana fisica; el BMIDE leeria la PRDT de una direccion basura.
  uint64_t prdt_phys = pmm_alloc_page();
  if (prdt_phys == 0 || prdt_phys >= 0x100000000ULL) {
    LOG_ERR("[ATA-DMA] no hay pagina fisica < 4GB para la PRDT");
    if (prdt_phys)
      pmm_free_page(prdt_phys);
    return -1;
  }
  dma->prdt = (ata_dma_prd_t *)phys_to_virt(prdt_phys);
  dma->prdt_phys = prdt_phys;
  memset(dma->prdt, 0, PAGE_SIZE);

  // 3. Inicializar lock y wait queue.
  spin_init(&dma->lock);
  wait_queue_init(&dma->irq_wq);
  dma->irq_pending = 0;
  dma->irq_error = 0;
  dma->irq_count = 0;
  dma->timeout_count = 0;
  dma->error_count = 0;

  // 4. Parar el BMIDE.
  outb(dma->bmide_base + BM_CMD, 0);

  // 5. Limpiar el status.
  outb(dma->bmide_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERROR);

  dma->initialized = 1;
  return 0;
}

int ata_dma_is_ready(int channel_idx) {
  if (channel_idx < 0 || channel_idx > 1)
    return 0;
  return g_dma[channel_idx].initialized;
}

// ===========================================================================
// Handler de IRQ
// ===========================================================================
void ata_dma_irq_handler(int channel_idx) {
  if (channel_idx < 0 || channel_idx > 1)
    return;

  ata_dma_state_t *dma = &g_dma[channel_idx];
  if (!dma->initialized)
    return;

  // [FIX] Leer BM_STATUS PRIMERO. Es la fuente de verdad de "esta IRQ es
  // mía". Si el BMIDE no tiene IRQ ni error pendiente, la IRQ es
  // compartida o espuria, y no tocamos nada.
  uint8_t bm = inb(dma->bmide_base + BM_STATUS);
  if (!(bm & (BM_STATUS_IRQ | BM_STATUS_ERROR))) {
    // IRQ no es del BMIDE. Puede ser de otro dispositivo que comparte
    // la línea (SMBus, etc). Salir sin tocar nada.
    return;
  }

  // Limpiar los bits del BMIDE (esto desaserta la línea del IOAPIC).
  outb(dma->bmide_base + BM_STATUS, bm | BM_STATUS_IRQ | BM_STATUS_ERROR);

  // [FIX] NO descartar por disk_status == 0x00. En VirtualBox PIIX3, el
  // disco baja DRDY/DRQ inmediatamente tras completar un WRITE_DMA, y
  // leer 0x00 NO significa IRQ espuria: significa que el disco terminó.
  // El código anterior descartaba la IRQ y dejaba al waiter colgado
  // hasta el timeout de 5 s.
  //
  // Solo descartamos si el bus está claramente flotante (0xFF), que
  // indica que el controlador no responde.
  uint16_t io = (channel_idx == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
  uint8_t disk_status = inb(io + ATA_REG_STATUS);
  if (disk_status == 0xFF) {
    // Bus flotante: no hay dispositivo. La IRQ es espuria, ya limpiamos
    // el BMIDE arriba, salimos sin despertar a nadie.
    return;
  }

  // ---------------------------------------------------------------------
  // Actualizar el estado de completación bajo el mismo lock que usa
  // wait_common para comprobar la condición y encolarse. Cierra la
  // ventana en la que el IRQ llegaba justo después de que el waiter
  // hubiese comprobado cond()==false pero antes de encolarse.
  // ---------------------------------------------------------------------
  unsigned long flags = spin_lock_irqsave(&dma->irq_wq.lock);

  dma->irq_pending = 1;
  dma->irq_error = (bm & BM_STATUS_ERROR) ? 1 : 0;
  dma->irq_count++;

  wake_up_all_locked(&dma->irq_wq);

  spin_unlock_irqrestore(&dma->irq_wq.lock, flags);
}

// ===========================================================================
// Condición para esperar la IRQ.
// ===========================================================================
static bool ata_dma_irq_cond(void *arg) {
  ata_dma_state_t *dma = (ata_dma_state_t *)arg;
  return dma->irq_pending != 0;
}

// ===========================================================================
// Construcción de la PRDT
// ===========================================================================
//
// El buffer es contiguo en virtual pero NO necesariamente en fisico, asi que
// se recorre pagina a pagina y se emite una entrada por tramo. Ademas cada
// entrada no puede cruzar un limite de 64 KB fisico ni superar 4 GB.
static int ata_dma_build_prdt(ata_dma_state_t *dma, const void *buf,
                              uint32_t byte_count) {
  uint64_t virt = (uint64_t)(uintptr_t)buf;
  uint32_t left = byte_count;
  unsigned n = 0;

  while (left > 0) {
    if (n >= ATA_DMA_MAX_PRD)
      return -1;

    uint64_t phys = paging_get_phys(virt);
    if (phys == 0)
      return -1;

    uint32_t chunk = PAGE_SIZE - (uint32_t)(virt & (PAGE_SIZE - 1));
    uint32_t to_64k = 0x10000 - (uint32_t)(phys & 0xFFFF);
    if (chunk > to_64k)
      chunk = to_64k;
    if (chunk > left)
      chunk = left;
    if (phys + chunk > 0x100000000ULL)
      return -1;

    dma->prdt[n].base = (uint32_t)phys;
    dma->prdt[n].count = (uint16_t)chunk; // chunk <= 4096: nunca 0 ni 64 KB
    dma->prdt[n].flags = 0;
    n++;
    virt += chunk;
    left -= chunk;
  }

  dma->prdt[n - 1].flags = ATA_DMA_PRD_EOT;
  return 0;
}

// ===========================================================================
// Preparar el BMIDE
// ===========================================================================
static void ata_dma_setup_bmide(ata_dma_state_t *dma, int is_read) {
  // 1. Parar el BMIDE.
  outb(dma->bmide_base + BM_CMD, 0);

  // 2. Limpiar el status.
  outb(dma->bmide_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERROR);

  // 3. Escribir la dirección física de la PRDT.
  outl(dma->bmide_base + BM_PRDT, (uint32_t)dma->prdt_phys);

  // 4. BM Command: solo la direccion (READ = el disco escribe en memoria).
  //    START se activa DESPUES de emitir el comando ATA (ata_dma_start_bmide).
  outb(dma->bmide_base + BM_CMD, is_read ? BM_CMD_READ : 0);

  ata_dma_delay();
}

static void ata_dma_start_bmide(ata_dma_state_t *dma, int is_read) {
  uint8_t cmd = BM_CMD_START_STOP;
  if (is_read)
    cmd |= BM_CMD_READ;
  outb(dma->bmide_base + BM_CMD, cmd);
}

// ===========================================================================
// Limpiar el BMIDE
// ===========================================================================
static void ata_dma_cleanup_bmide(ata_dma_state_t *dma) {
  outb(dma->bmide_base + BM_CMD, 0);
  outb(dma->bmide_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERROR);
}

// ===========================================================================
// Enviar un comando DMA al disco (LBA48 o LBA28).
// ===========================================================================
static int ata_dma_issue_cmd(struct ata_device *dev, uint64_t lba,
                             uint32_t count, uint8_t cmd) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  if (dev->use_lba48) {
    // LBA48.
    uint8_t drive_head = ATA_DRIVE_LBA | (dev->drive << 4);
    outb(io + ATA_REG_DRIVE, drive_head);
    ata_io_delay(ctrl);

    rc = ata_wait_not_busy(io, timeout);
    if (rc != ATA_OK)
      return rc;

    outb(io + ATA_REG_SECCOUNT, (uint8_t)(count >> 8));
    outb(io + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
    outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 32) & 0xFF));
    outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 40) & 0xFF));
    ata_io_delay(ctrl);

    rc = ata_wait_not_busy(io, timeout);
    if (rc != ATA_OK)
      return rc;

    outb(io + ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));
    outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    ata_io_delay(ctrl);

    rc = ata_wait_not_busy(io, timeout);
    if (rc != ATA_OK)
      return rc;
  } else {
    // LBA28.
    uint8_t drive_head = 0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F);
    outb(io + ATA_REG_DRIVE, drive_head);
    ata_io_delay(ctrl);

    rc = ata_wait_not_busy(io, timeout);
    if (rc != ATA_OK)
      return rc;

    if (count > 255)
      count = 255;
    outb(io + ATA_REG_SECCOUNT, (uint8_t)count);
    ata_io_delay(ctrl);
    outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    ata_io_delay(ctrl);
    outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    ata_io_delay(ctrl);
    outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    ata_io_delay(ctrl);

    rc = ata_wait_not_busy(io, timeout);
    if (rc != ATA_OK)
      return rc;
  }

  outb(io + ATA_REG_COMMAND, cmd);
  return ATA_OK;
}

// ===========================================================================
// Operación DMA común.
//
// [FIX SMP + TIMEOUT REAL + ESCRITURA]
//
//   - Espera la IRQ del BMIDE con wait_event_interruptible_timeout (o
//     polling si estamos en contexto atómico con preempt_count > 0).
//
//   - Tras la IRQ, en ESCRITURAS, espera a que el disco termine de
//     escribir los datos al medio (BSY=0). La IRQ del BMIDE se genera
//     cuando el bus master termina de transferir los datos a su buffer
//     interno, NO cuando el disco ha completado la escritura al medio.
//     Sin esta espera, el siguiente comando (flush, read de vuelta)
//     puede ejecutarse antes de que los datos estén en el medio, y el
//     test lee datos viejos. QEMU idealiza este timing; VirtualBox lo
//     emula fielmente y por eso el bug solo aparece en VirtualBox.
// ===========================================================================
static int ata_dma_xfer(struct ata_device *dev, uint64_t lba, uint32_t count,
                        void *buf, int is_read) {
  int ch_idx = (dev->channel->io_base == ATA_PRIMARY_IO) ? 0 : 1;
  ata_dma_state_t *dma = &g_dma[ch_idx];
  if (!dma->initialized)
    return ATA_ERR_UNKNOWN;

  uint32_t byte_count = count * dev->sector_size;
  if (byte_count == 0 || byte_count > 0x10000) {
    LOG_ERR("[ATA-DMA] transferencia de %u bytes no soportada (max 64 KB)",
            byte_count);
    return ATA_ERR_UNKNOWN;
  }

  if (((uintptr_t)buf & 0x1) != 0) {
    LOG_ERR("[ATA-DMA] buffer no alineado a 2 bytes: %p", buf);
    return ATA_ERR_UNKNOWN;
  }

  // =========================================================================
  // Fase 1: preparar PRDT + BMIDE + comando ATA bajo dma->lock.
  //
  // dma->lock protege los registros del BMIDE (incluido el PRDT), no el
  // estado de completación. El estado de completación se protege con
  // dma->irq_wq.lock, que es el que usa el IRQ handler.
  //
  // Orden de locks (siempre en este orden): dma->lock → dma->irq_wq.lock.
  // Nunca al revés.
  // =========================================================================
  unsigned long flags = spin_lock_irqsave(&dma->lock);

  if (ata_dma_build_prdt(dma, buf, byte_count) != 0) {
    LOG_ERR("[ATA-DMA] no se pudo construir la PRDT para %p (%u bytes)", buf,
            byte_count);
    spin_unlock_irqrestore(&dma->lock, flags);
    return ATA_ERR_UNKNOWN;
  }

  DMA_LOG("[ATA-DMA] xfer: lba=%lu count=%u buf=%p read=%d", (unsigned long)lba,
          count, buf, is_read);

  ata_dma_setup_bmide(dma, is_read);

  uint8_t cmd;
  if (dev->use_lba48) {
    cmd = is_read ? ATA_CMD_READ_DMA_EXT : ATA_CMD_WRITE_DMA_EXT;
  } else {
    cmd = is_read ? ATA_CMD_READ_DMA : ATA_CMD_WRITE_DMA;
  }

  int rc = ata_dma_issue_cmd(dev, lba, count, cmd);
  if (rc != ATA_OK) {
    LOG_ERR("[ATA-DMA] issue_cmd falló: %d", rc);
    ata_dma_cleanup_bmide(dma);
    spin_unlock_irqrestore(&dma->lock, flags);
    return rc;
  }

  // -------------------------------------------------------------------------
  // [FIX race SMP] Resetear el estado de completación bajo el MISMO lock
  // que el IRQ handler. Todavía no hemos arrancado el bus master, así
  // que ningún IRQ puede dispararse en este punto, pero usamos el lock
  // correcto para que el estado sea coherente con las lecturas que
  // haremos en Fase 3.
  // -------------------------------------------------------------------------
  unsigned long wf = spin_lock_irqsave(&dma->irq_wq.lock);
  dma->irq_pending = 0;
  dma->irq_error = 0;
  uint32_t irq_count_before = dma->irq_count;
  spin_unlock_irqrestore(&dma->irq_wq.lock, wf);

  // Arrancar el bus master. A partir de aquí el IRQ puede dispararse en
  // cualquier núcleo. Seguimos con dma->lock cogido para serializar el
  // acceso al registro BM_CMD.
  ata_dma_start_bmide(dma, is_read);

  spin_unlock_irqrestore(&dma->lock, flags);

  // =========================================================================
  // Fase 2: dormir hasta que la IRQ marque irq_pending, con timeout real.
  //
  // Las IRQs están HABILITADAS durante esta espera. El lock de la wait
  // queue se toma y se suelta dentro de wait_common solo durante los
  // breves instantes en los que se comprueba la condición y se encola/
  // desencola la tarea. El sched_yield() posterior ocurre con IRQs
  // habilitadas, por lo que el LAPIC timer y el IRQ handler del DMA
  // pueden ejecutarse en cualquier núcleo.
  // =========================================================================
  uint64_t timeout_ticks = (uint64_t)ATA_DMA_TIMEOUT_SECONDS * 1000ULL;

  long w;
  if (preempt_count() > 0) {
    // Contexto atómico: no podemos dormir, hacemos polling. La IRQ
    // puede llegar igualmente (preempt_count no enmascara IRQs), así
    // que irq_pending se pondrá a 1 tarde o temprano.
    uint64_t deadline = sched_get_ticks() + timeout_ticks;
    w = 0;
    while (!dma->irq_pending) {
      if (sched_get_ticks() >= deadline)
        break;
      __asm__ volatile("pause");
    }
    if (dma->irq_pending)
      w = 1;
  } else {
    // El lock wq->lock se toma/suelta internamente; el sueño ocurre con
    // IRQs habilitadas.
    w = wait_event_interruptible_timeout(&dma->irq_wq, ata_dma_irq_cond, dma,
                                         timeout_ticks);
  }

  // =========================================================================
  // Fase 3: evaluar el resultado.
  //
  // Tomamos dma->lock para el cleanup del BMIDE y las operaciones sobre
  // los registros del disco. El estado de completación (irq_count,
  // irq_error) lo leemos bajo dma->irq_wq.lock, consistente con el
  // handler y con la Fase 1.
  //
  // Orden: dma->lock → dma->irq_wq.lock (igual que en Fase 1).
  // =========================================================================
  flags = spin_lock_irqsave(&dma->lock);

  wf = spin_lock_irqsave(&dma->irq_wq.lock);
  uint32_t irq_count_after = dma->irq_count;
  int irq_err = dma->irq_error;
  spin_unlock_irqrestore(&dma->irq_wq.lock, wf);

  if (w == 0 && irq_count_after == irq_count_before) {
    LOG_ERR("[ATA-DMA] timeout esperando IRQ (canal %d, %u timeouts totales)",
            ch_idx, dma->timeout_count);
    dma->timeout_count++;
    ata_dma_cleanup_bmide(dma);
    spin_unlock_irqrestore(&dma->lock, flags);
    return ATA_ERR_TIMEOUT;
  }

  if (w < 0) {
    LOG_WARN("[ATA-DMA] espera interrumpida (canal %d)", ch_idx);
    ata_dma_cleanup_bmide(dma);
    spin_unlock_irqrestore(&dma->lock, flags);
    return ATA_ERR_TIMEOUT;
  }

  if (irq_err) {
    LOG_ERR("[ATA-DMA] error del BMIDE (canal %d)", ch_idx);
    dma->error_count++;
    ata_dma_cleanup_bmide(dma);
    spin_unlock_irqrestore(&dma->lock, flags);
    return ATA_ERR_UNC;
  }

  uint16_t io = dev->channel->io_base;
  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status & ATA_SR_ERR) {
    rc = ata_decode_error(io);
    LOG_ERR("[ATA-DMA] error del disco: %d", rc);
    dma->error_count++;
    ata_dma_cleanup_bmide(dma);
    spin_unlock_irqrestore(&dma->lock, flags);
    return rc;
  }

  // -------------------------------------------------------------------------
  // [FIX VirtualBox] Tras una ESCRITURA DMA, forzar un FLUSH CACHE para
  // asegurar que los datos han llegado al medio antes de dar la operación
  // por completada.
  //
  // VirtualBox emula el PIIX3 de forma que la IRQ de finalización de DMA
  // puede llegar antes de que el disco haya persistido los datos. Aunque
  // esperemos a BSY=0, el disco puede haber liberado BSY sin haber
  // flusheado su caché interna. El siguiente comando (read de vuelta)
  // lee datos obsoletos.
  // -------------------------------------------------------------------------
  if (!is_read) {
    ata_select_drive(dev->channel, dev->drive);

    rc = ata_wait_not_busy(io, ata_timeout_iters());
    if (rc != ATA_OK) {
      LOG_WARN("[ATA-DMA] disco no responde antes de FLUSH: %d", rc);
      ata_dma_cleanup_bmide(dma);
      spin_unlock_irqrestore(&dma->lock, flags);
      return rc;
    }

    uint8_t flush_cmd =
        dev->use_lba48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE;
    outb(io + ATA_REG_COMMAND, flush_cmd);

    rc = ata_wait_not_busy(io, ata_timeout_iters());
    if (rc != ATA_OK) {
      LOG_WARN("[ATA-DMA] FLUSH CACHE falló tras escritura: %d", rc);
      ata_dma_cleanup_bmide(dma);
      spin_unlock_irqrestore(&dma->lock, flags);
      return rc;
    }

    uint8_t flush_status = inb(io + ATA_REG_STATUS);
    if (flush_status & ATA_SR_ERR) {
      rc = ata_decode_error(io);
      LOG_WARN("[ATA-DMA] error en FLUSH CACHE: %d", rc);
      ata_dma_cleanup_bmide(dma);
      spin_unlock_irqrestore(&dma->lock, flags);
      return rc;
    }
  }

  ata_dma_cleanup_bmide(dma);
  spin_unlock_irqrestore(&dma->lock, flags);
  return ATA_OK;
}

// Divide en trozos de <= 64 KB (limite de ata_dma_xfer).
static int ata_dma_rw(struct ata_device *dev, uint64_t lba, uint32_t count,
                      void *buf, int is_read) {
  uint32_t max = 0x10000 / dev->sector_size;
  uint8_t *p = (uint8_t *)buf;

  while (count > 0) {
    uint32_t n = (count > max) ? max : count;
    int rc = ata_dma_xfer(dev, lba, n, p, is_read);
    if (rc != ATA_OK)
      return rc;
    lba += n;
    p += (uint64_t)n * dev->sector_size;
    count -= n;
  }
  return ATA_OK;
}

int ata_dma_read(struct ata_device *dev, uint64_t lba, uint32_t count,
                 void *buf) {
  return ata_dma_rw(dev, lba, count, buf, 1);
}

int ata_dma_write(struct ata_device *dev, uint64_t lba, uint32_t count,
                  const void *buf) {
  return ata_dma_rw(dev, lba, count, (void *)buf, 0);
}

// ===========================================================================
// Handlers específicos de IRQ (para idt.c).
// ===========================================================================
void ata_dma_irq14(void) { ata_dma_irq_handler(0); }
void ata_dma_irq15(void) { ata_dma_irq_handler(1); }

// ===========================================================================
// Debug
// ===========================================================================
void ata_dma_dump(void) {
  for (int i = 0; i < 2; i++) {
    ata_dma_state_t *dma = &g_dma[i];
    if (!dma->initialized)
      continue;
    LOG_INFO("[ATA-DMA] Canal %s: base=0x%x IRQ=%u",
             i == 0 ? "primario" : "secundario", dma->bmide_base, dma->irq);
    LOG_INFO("[ATA-DMA]   IRQs: %u, timeouts: %u, errores: %u", dma->irq_count,
             dma->timeout_count, dma->error_count);
  }
}