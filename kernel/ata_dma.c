// kernel/ata_dma.c
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

// Bounce buffers (path sync). Ver ata_dma_xfer para contexto.
static uint8_t g_bounce[2][0x10000] __attribute__((aligned(4096)));

#define BM_CMD 0x00
#define BM_STATUS 0x02
#define BM_PRDT 0x04

#define ATA_DMA_MAX_PRD (PAGE_SIZE / sizeof(ata_dma_prd_t))

static void ata_dma_delay(void) {
  for (volatile int i = 0; i < 100; i++)
    __asm__ volatile("pause");
}

// ===========================================================================
// Init
// ===========================================================================
int ata_dma_init_channel(int channel_idx, uint8_t pci_bus, uint8_t pci_slot,
                         uint8_t pci_func) {
  if (channel_idx < 0 || channel_idx > 1)
    return -1;

  ata_dma_state_t *dma = &g_dma[channel_idx];
  memset(dma, 0, sizeof(*dma));

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

  spin_init(&dma->lock);
  wait_queue_init(&dma->irq_wq);
  dma->irq_pending = 0;
  dma->irq_error = 0;
  dma->irq_count = 0;
  dma->timeout_count = 0;
  dma->error_count = 0;

  // [FASE 1.4] Estado async limpio.
  dma->inflight_bio = NULL;
  dma->xfer_state = XFS_IDLE;

  outb(dma->bmide_base + BM_CMD, 0);
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
// PRDT build
// ===========================================================================
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
    dma->prdt[n].count = (uint16_t)chunk;
    dma->prdt[n].flags = 0;
    n++;
    virt += chunk;
    left -= chunk;
  }

  dma->prdt[n - 1].flags = ATA_DMA_PRD_EOT;
  return 0;
}

// ===========================================================================
// BMIDE setup / start / cleanup
// ===========================================================================
static void ata_dma_setup_bmide(ata_dma_state_t *dma, int is_read) {
  outb(dma->bmide_base + BM_CMD, 0);
  outb(dma->bmide_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERROR);
  outl(dma->bmide_base + BM_PRDT, (uint32_t)dma->prdt_phys);
  outb(dma->bmide_base + BM_CMD, is_read ? BM_CMD_READ : 0);
  ata_dma_delay();
}

static void ata_dma_start_bmide(ata_dma_state_t *dma, int is_read) {
  uint8_t cmd = BM_CMD_START_STOP;
  if (is_read)
    cmd |= BM_CMD_READ;
  outb(dma->bmide_base + BM_CMD, cmd);
}

static void ata_dma_cleanup_bmide(ata_dma_state_t *dma) {
  outb(dma->bmide_base + BM_CMD, 0);
  outb(dma->bmide_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERROR);
}

// ===========================================================================
// Issue ATA command (shared by sync + async)
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
// SYNC path (unchanged, only used for odd buffers and PIO fallback)
// ===========================================================================
static bool ata_dma_irq_cond(void *arg) {
  ata_dma_state_t *dma = (ata_dma_state_t *)arg;
  return dma->irq_pending != 0;
}

static int ata_dma_xfer(struct ata_device *dev, uint64_t lba, uint32_t count,
                        void *buf, int is_read) {
  int ch_idx = (dev->channel->io_base == ATA_PRIMARY_IO) ? 0 : 1;
  ata_dma_state_t *dma = &g_dma[ch_idx];
  if (!dma->initialized)
    return ATA_ERR_UNKNOWN;

  uint32_t byte_count = count * dev->sector_size;
  if (byte_count == 0 || byte_count > 0x10000) {
    LOG_ERR("[ATA-DMA] transferencia de %u bytes no soportada", byte_count);
    return ATA_ERR_UNKNOWN;
  }

  uint8_t *bounce = NULL;
  void *dma_buf = buf;
  if (((uintptr_t)buf & 1) != 0) {
    if (byte_count > sizeof(g_bounce[0])) {
      LOG_ERR("[ATA-DMA] buffer impar demasiado grande para bounce: %u",
              byte_count);
      return ATA_ERR_UNKNOWN;
    }
    bounce = g_bounce[ch_idx];
    dma_buf = bounce;
  }

  unsigned long flags = spin_lock_irqsave(&dma->lock);

  if (bounce && !is_read) {
    memcpy(bounce, buf, byte_count);
  }

  if (ata_dma_build_prdt(dma, dma_buf, byte_count) != 0) {
    spin_unlock_irqrestore(&dma->lock, flags);
    return ATA_ERR_UNKNOWN;
  }

  ata_dma_setup_bmide(dma, is_read);

  uint8_t cmd;
  if (dev->use_lba48) {
    cmd = is_read ? ATA_CMD_READ_DMA_EXT : ATA_CMD_WRITE_DMA_EXT;
  } else {
    cmd = is_read ? ATA_CMD_READ_DMA : ATA_CMD_WRITE_DMA;
  }

  int rc = ata_dma_issue_cmd(dev, lba, count, cmd);
  if (rc != ATA_OK) {
    ata_dma_cleanup_bmide(dma);
    spin_unlock_irqrestore(&dma->lock, flags);
    return rc;
  }

  unsigned long wf = spin_lock_irqsave(&dma->irq_wq.lock);
  dma->irq_pending = 0;
  dma->irq_error = 0;
  uint32_t irq_count_before = dma->irq_count;
  spin_unlock_irqrestore(&dma->irq_wq.lock, wf);

  ata_dma_start_bmide(dma, is_read);
  spin_unlock_irqrestore(&dma->lock, flags);

  uint64_t timeout_ticks = (uint64_t)ATA_DMA_TIMEOUT_SECONDS * 1000ULL;
  long w;
  if (preempt_count() > 0) {
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
    w = wait_event_interruptible_timeout(&dma->irq_wq, ata_dma_irq_cond, dma,
                                         timeout_ticks);
  }

  flags = spin_lock_irqsave(&dma->lock);

  wf = spin_lock_irqsave(&dma->irq_wq.lock);
  uint32_t irq_count_after = dma->irq_count;
  int irq_err = dma->irq_error;
  spin_unlock_irqrestore(&dma->irq_wq.lock, wf);

  if (w == 0 && irq_count_after == irq_count_before) {
    LOG_ERR("[ATA-DMA] timeout esperando IRQ (canal %d)", ch_idx);
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
      LOG_WARN("[ATA-DMA] FLUSH CACHE falló: %d", rc);
      ata_dma_cleanup_bmide(dma);
      spin_unlock_irqrestore(&dma->lock, flags);
      return rc;
    }
    uint8_t fs = inb(io + ATA_REG_STATUS);
    if (fs & ATA_SR_ERR) {
      rc = ata_decode_error(io);
      ata_dma_cleanup_bmide(dma);
      spin_unlock_irqrestore(&dma->lock, flags);
      return rc;
    }
  }

  if (bounce && is_read) {
    memcpy(buf, bounce, byte_count);
  }

  ata_dma_cleanup_bmide(dma);
  spin_unlock_irqrestore(&dma->lock, flags);
  return ATA_OK;
}

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
// [FASE 1.4] ASYNC path
// ===========================================================================

// Arranca el siguiente chunk del bio en vuelo. Asume dma->lock cogido.
// Retorna 0 si el chunk se lanzó (IRQ disparará), <0 si error.
static int ata_dma_start_chunk_locked(ata_dma_state_t *dma,
                                      struct ata_device *dev) {
  if (dma->xfer_left == 0)
    return -EINVAL;

  uint32_t n = dma->xfer_left;
  uint32_t max = 0x10000 / dev->sector_size;
  if (n > max)
    n = max;

  uint32_t bytes = n * dev->sector_size;
  if (ata_dma_build_prdt(dma, dma->xfer_buf, bytes) != 0) {
    return -EIO;
  }

  ata_dma_setup_bmide(dma, dma->xfer_is_read);

  uint8_t cmd;
  if (dev->use_lba48) {
    cmd = dma->xfer_is_read ? ATA_CMD_READ_DMA_EXT : ATA_CMD_WRITE_DMA_EXT;
  } else {
    cmd = dma->xfer_is_read ? ATA_CMD_READ_DMA : ATA_CMD_WRITE_DMA;
  }

  int rc = ata_dma_issue_cmd(dev, dma->xfer_lba, n, cmd);
  if (rc != ATA_OK) {
    ata_dma_cleanup_bmide(dma);
    return rc;
  }

  // Limpiar IRQ state ANTES de arrancar BMIDE. Lock order:
  // dma->lock -> dma->irq_wq.lock (consistente con el IRQ handler).
  unsigned long wf = spin_lock_irqsave(&dma->irq_wq.lock);
  dma->irq_pending = 0;
  dma->irq_error = 0;
  spin_unlock_irqrestore(&dma->irq_wq.lock, wf);

  ata_dma_start_bmide(dma, dma->xfer_is_read);

  dma->xfer_chunk_sectors = n;
  dma->last_cmd_tick = sched_get_ticks();
  return 0;
}

// Envía FLUSH CACHE EXT/FLUSH CACHE. Asume dma->lock NO cogido
// (bloquea en ata_wait_not_busy).
static int ata_dma_send_flush(struct ata_device *dev) {
  uint16_t io = dev->channel->io_base;
  uint64_t timeout = ata_timeout_iters();

  int rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  ata_select_drive(dev->channel, dev->drive);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  uint8_t cmd = dev->use_lba48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE;
  outb(io + ATA_REG_COMMAND, cmd);
  return 0;
}

// Submit asíncrono. Devuelve 0 si arrancó con error (bio NO en vuelo),
// -EINPROGRESS si arrancó OK, <0 si error (no arrancó).
int ata_dma_submit_async(struct ata_device *dev, struct bio *bio) {
  int ch_idx = (dev->channel->io_base == ATA_PRIMARY_IO) ? 0 : 1;
  ata_dma_state_t *dma = &g_dma[ch_idx];
  if (!dma->initialized)
    return -ENODEV;

  // Caso especial: BIO_FLUSH no tiene datos.
  if (bio->op == BIO_FLUSH) {
    ata_chan_acquire(dev->channel);

    unsigned long flags = spin_lock_irqsave(&dma->lock);
    if (dma->inflight_bio != NULL) {
      spin_unlock_irqrestore(&dma->lock, flags);
      ata_chan_release(dev->channel);
      return -EBUSY;
    }
    dma->inflight_bio = bio;
    dma->inflight_dev = dev;
    dma->inflight_sector_size = bio->bdev->sector_size;
    dma->xfer_state = XFS_FLUSHING;
    dma->last_cmd_tick = sched_get_ticks();
    spin_unlock_irqrestore(&dma->lock, flags);

    if (ata_dma_send_flush(dev) != 0) {
      flags = spin_lock_irqsave(&dma->lock);
      dma->inflight_bio = NULL;
      dma->xfer_state = XFS_IDLE;
      spin_unlock_irqrestore(&dma->lock, flags);
      ata_chan_release(dev->channel);
      return -EIO;
    }

    bio->error = 0;
    return -EINPROGRESS;
  }

  // Data transfer (READ o WRITE).
  ata_chan_acquire(dev->channel);

  unsigned long flags = spin_lock_irqsave(&dma->lock);
  if (dma->inflight_bio != NULL) {
    spin_unlock_irqrestore(&dma->lock, flags);
    ata_chan_release(dev->channel);
    return -EBUSY;
  }
  dma->inflight_bio = bio;
  dma->inflight_dev = dev;
  dma->inflight_sector_size = bio->bdev->sector_size;
  dma->xfer_lba = bio->lba;
  dma->xfer_buf = (uint8_t *)bio->buf;
  dma->xfer_left = bio->count;
  dma->xfer_is_read = (bio->op == BIO_READ) ? 1 : 0;
  dma->xfer_need_flush = (bio->op == BIO_WRITE) ? 1 : 0;
  dma->xfer_error = 0;
  dma->xfer_state = XFS_RUNNING;
  dma->xfer_chunk_sectors = 0;

  int rc = ata_dma_start_chunk_locked(dma, dev);
  if (rc != 0) {
    dma->inflight_bio = NULL;
    dma->xfer_state = XFS_IDLE;
    spin_unlock_irqrestore(&dma->lock, flags);
    ata_chan_release(dev->channel);
    return rc;
  }

  spin_unlock_irqrestore(&dma->lock, flags);
  bio->error = 0;
  return -EINPROGRESS;
}

// Procesa el IRQ (o el polling rescue) del path async.
static void ata_dma_handle_async_irq(ata_dma_state_t *dma, int ch_idx) {
  struct bio *bio = NULL;
  struct ata_device *dev = NULL;
  int error = 0;
  int need_next_chunk = 0;
  int need_flush_now = 0;
  int need_release = 0;

  unsigned long flags = spin_lock_irqsave(&dma->lock);

  bio = dma->inflight_bio;
  if (!bio) {
    spin_unlock_irqrestore(&dma->lock, flags);
    return;
  }
  dev = dma->inflight_dev; // no bio->bdev: ver ata_dma.h

  int state = dma->xfer_state;

  if (state == XFS_RUNNING) {
    uint8_t bm = inb(dma->bmide_base + BM_STATUS);
    uint16_t io = (ch_idx == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
    uint8_t disk_status = inb(io + ATA_REG_STATUS);

    int is_err =
        (bm & BM_STATUS_ERROR) || (disk_status & ATA_SR_ERR) || dma->irq_error;

    if (is_err) {
      error = -EIO;
      need_release = 1;
      dma->inflight_bio = NULL;
      dma->xfer_state = XFS_IDLE;
      ata_dma_cleanup_bmide(dma);
    } else {
      uint32_t consumed = dma->xfer_chunk_sectors;
      dma->xfer_lba += consumed;
      dma->xfer_left -= consumed;
      dma->xfer_buf += (uint64_t)consumed * dma->inflight_sector_size;

      if (dma->xfer_left > 0) {
        need_next_chunk = 1;
      } else if (dma->xfer_need_flush) {
        dma->xfer_need_flush = 0;
        dma->xfer_state = XFS_FLUSHING;
        dma->last_cmd_tick = sched_get_ticks();
        need_flush_now = 1;
      } else {
        error = 0;
        need_release = 1;
        dma->inflight_bio = NULL;
        dma->xfer_state = XFS_IDLE;
      }
    }
  } else if (state == XFS_FLUSHING) {
    uint16_t io = (ch_idx == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
    uint8_t disk_status = inb(io + ATA_REG_STATUS);
    error = (disk_status & ATA_SR_ERR) ? -EIO : 0;
    need_release = 1;
    dma->inflight_bio = NULL;
    dma->xfer_state = XFS_IDLE;
  } else {
    spin_unlock_irqrestore(&dma->lock, flags);
    return;
  }

  spin_unlock_irqrestore(&dma->lock, flags);

  if (need_next_chunk) {
    unsigned long f2 = spin_lock_irqsave(&dma->lock);
    int rc = ata_dma_start_chunk_locked(dma, dev);
    spin_unlock_irqrestore(&dma->lock, f2);
    if (rc != 0) {
      f2 = spin_lock_irqsave(&dma->lock);
      dma->inflight_bio = NULL;
      dma->xfer_state = XFS_IDLE;
      spin_unlock_irqrestore(&dma->lock, f2);
      ata_chan_release(dev->channel);
      bio_endio(bio, -EIO);
    }
    return;
  }

  if (need_flush_now) {
    if (ata_dma_send_flush(dev) != 0) {
      unsigned long f3 = spin_lock_irqsave(&dma->lock);
      dma->inflight_bio = NULL;
      dma->xfer_state = XFS_IDLE;
      spin_unlock_irqrestore(&dma->lock, f3);
      ata_chan_release(dev->channel);
      bio_endio(bio, -EIO);
    }
    return;
  }

  if (need_release && bio) {
    ata_chan_release(dev->channel);
    bio_endio(bio, error);
  }
}

// ===========================================================================
// IRQ handler (sync + async)
// ===========================================================================
void ata_dma_irq_handler(int channel_idx) {
  if (channel_idx < 0 || channel_idx > 1)
    return;
  ata_dma_state_t *dma = &g_dma[channel_idx];
  if (!dma->initialized)
    return;

  // Leer BM_STATUS ANTES de cualquier otra cosa. El BMIDE es el dueño
  // del bit IRQ en su registro. Si está limpio, la IRQ es de otro
  // dispositivo (SMBus compartiendo línea).
  uint8_t bm = inb(dma->bmide_base + BM_STATUS);
  int is_bmide = (bm & (BM_STATUS_IRQ | BM_STATUS_ERROR)) != 0;

  if (is_bmide) {
    outb(dma->bmide_base + BM_STATUS, bm | BM_STATUS_IRQ | BM_STATUS_ERROR);
  }

  // Ver el estado antes de hacer nada: si es FLUSHING, la IRQ es del
  // FLUSH CACHE (BMIDE no interviene), no del BMIDE.
  unsigned long flags = spin_lock_irqsave(&dma->lock);
  int state = dma->xfer_state;
  spin_unlock_irqrestore(&dma->lock, flags);

  if (state == XFS_FLUSHING) {
    // FLUSH: no toca BMIDE, solo despertar/async-complete.
    flags = spin_lock_irqsave(&dma->irq_wq.lock);
    dma->irq_pending = 1;
    dma->irq_error = 0;
    dma->irq_count++;
    wake_up_all_locked(&dma->irq_wq);
    spin_unlock_irqrestore(&dma->irq_wq.lock, flags);

    ata_dma_handle_async_irq(dma, channel_idx);
    return;
  }

  if (!is_bmide) {
    // Sin BMIDE IRQ y sin FLUSHING: IRQ ajena, salir.
    return;
  }

  // Data IRQ.
  uint16_t io = (channel_idx == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
  uint8_t disk_status = inb(io + ATA_REG_STATUS);
  if (disk_status == 0xFF) {
    // Bus flotante.
    return;
  }

  flags = spin_lock_irqsave(&dma->irq_wq.lock);
  dma->irq_pending = 1;
  dma->irq_error = (bm & BM_STATUS_ERROR) ? 1 : 0;
  dma->irq_count++;
  wake_up_all_locked(&dma->irq_wq);
  spin_unlock_irqrestore(&dma->irq_wq.lock, flags);

  ata_dma_handle_async_irq(dma, channel_idx);
}

// ===========================================================================
// [FASE 1.4] Polling rescue
// ===========================================================================
void ata_dma_poll_ports(void) {
  uint64_t now = sched_get_ticks();

  for (int ch = 0; ch < 2; ch++) {
    ata_dma_state_t *dma = &g_dma[ch];
    if (!dma->initialized)
      continue;

    unsigned long flags = spin_lock_irqsave(&dma->lock);
    struct bio *bio = dma->inflight_bio;
    int state = dma->xfer_state;
    uint64_t last = dma->last_cmd_tick;
    spin_unlock_irqrestore(&dma->lock, flags);

    if (!bio || state == XFS_IDLE)
      continue;
    if (now - last < ATA_DMA_POLL_DELAY_TICKS)
      continue;

    // ¿El hardware terminó sin avisar por IRQ?
    if (state == XFS_RUNNING) {
      uint8_t bm = inb(dma->bmide_base + BM_STATUS);
      if (bm & BM_STATUS_ACTIVE)
        continue; // sigue transfiriendo
      // BMIDE sin active: forzar irq_pending (idempotente con IRQ real).
      flags = spin_lock_irqsave(&dma->irq_wq.lock);
      if (!dma->irq_pending) {
        dma->irq_pending = 1;
        dma->irq_error = (bm & BM_STATUS_ERROR) ? 1 : 0;
        dma->irq_count++;
      }
      spin_unlock_irqrestore(&dma->irq_wq.lock, flags);
      ata_dma_handle_async_irq(dma, ch);
    } else if (state == XFS_FLUSHING) {
      uint16_t io = (ch == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
      uint8_t disk_status = inb(io + ATA_REG_STATUS);
      if (disk_status & ATA_SR_BSY)
        continue; // aún ocupado
      flags = spin_lock_irqsave(&dma->irq_wq.lock);
      if (!dma->irq_pending) {
        dma->irq_pending = 1;
        dma->irq_error = 0;
        dma->irq_count++;
      }
      spin_unlock_irqrestore(&dma->irq_wq.lock, flags);
      ata_dma_handle_async_irq(dma, ch);
    }
  }
}

// ===========================================================================
// IRQ stubs
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