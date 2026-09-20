// kernel/ata_pio.c
//
// Driver ATA PIO. Usa los helpers de ata_common.
//
// Lee/escribe con PIO (LBA28 + LBA48).

#include "ata_pio.h"
#include "ata_common.h"
#include "block.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"

// ===========================================================================
// Estado de un disco ATA
// ===========================================================================
typedef struct ata_device {
  char model[41];
  char serial[21];
  char firmware[9];

  ata_channel_t *channel;
  uint8_t drive;
  uint8_t is_atapi;

  uint8_t supports_lba;
  uint8_t supports_lba48;
  uint8_t supports_dma;
  uint8_t supports_flush;
  uint8_t supports_ncq;
  uint16_t pio_modes;
  uint16_t dma_modes;
  uint16_t udma_modes;
  uint32_t sector_size;
  uint64_t lba28_sectors;
  uint64_t lba48_sectors;

  uint8_t use_lba48;
  uint8_t pio_mode;

  uint64_t num_sectors;
  uint32_t error_count;
  uint32_t retry_count;

  block_device_t *bdev;
  struct ata_device *next;
} ata_device_t;

// ===========================================================================
// Estado global
// ===========================================================================
static ata_channel_t g_channels[2];
static ata_device_t *g_devices = NULL;
static int g_device_count = 0;

// ===========================================================================
// Parseo del IDENTIFY
// ===========================================================================
static void ata_copy_string(char *dst, const uint16_t *words, int num_words,
                            size_t dst_size) {
  size_t j = 0;
  for (int i = 0; i < num_words; i++) {
    char hi = (char)(words[i] >> 8);
    char lo = (char)(words[i] & 0xFF);
    if (j + 1 < dst_size)
      dst[j++] = hi;
    if (j + 1 < dst_size)
      dst[j++] = lo;
  }
  dst[j] = '\0';
  while (j > 0 && (dst[j - 1] == ' ' || dst[j - 1] == '\0')) {
    dst[--j] = '\0';
  }
}

static uint32_t ata_words_to_u32(const uint16_t *w) {
  return ((uint32_t)w[1] << 16) | (uint32_t)w[0];
}

static uint64_t ata_words_to_u64(const uint16_t *w) {
  return ((uint64_t)w[3] << 48) | ((uint64_t)w[2] << 32) |
         ((uint64_t)w[1] << 16) | (uint64_t)w[0];
}

static void ata_parse_identify(ata_device_t *dev, const uint16_t *id) {
  ata_copy_string(dev->model, &id[27], 20, sizeof(dev->model));
  ata_copy_string(dev->serial, &id[10], 10, sizeof(dev->serial));
  ata_copy_string(dev->firmware, &id[23], 4, sizeof(dev->firmware));

  uint16_t caps = id[49];
  dev->supports_dma = (caps & (1 << 8)) ? 1 : 0;
  dev->supports_lba = (caps & (1 << 9)) ? 1 : 0;

  uint16_t caps83 = id[83];
  dev->supports_lba48 = (caps83 & (1 << 10)) ? 1 : 0;
  dev->supports_flush = (caps83 & (1 << 12)) ? 1 : 0;

  uint16_t pio64 = id[64];
  uint16_t pio65 = id[65];
  dev->pio_modes = 0;
  if (pio65 & (1 << 0))
    dev->pio_modes |= (1 << 0);
  if (pio65 & (1 << 1))
    dev->pio_modes |= (1 << 1);
  if (pio64 & (1 << 0))
    dev->pio_modes |= (1 << 2);
  if (pio64 & (1 << 1))
    dev->pio_modes |= (1 << 3);
  if (pio64 & (1 << 2))
    dev->pio_modes |= (1 << 4);

  dev->dma_modes = id[63] & 0x07;
  dev->udma_modes = id[88] & 0x7F;

  if (id[76] & (1 << 8))
    dev->supports_ncq = 1;

  dev->lba28_sectors = ata_words_to_u32(&id[60]);
  dev->lba48_sectors = ata_words_to_u64(&id[100]);

  uint16_t word106 = id[106];
  uint16_t logical_words = word106 & 0x0F;
  if (logical_words == 0) {
    dev->sector_size = 512;
  } else {
    dev->sector_size = 1u << (logical_words + 1);
  }
}

// ===========================================================================
// IDENTIFY DEVICE
// ===========================================================================
static int ata_identify(ata_device_t *dev) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint64_t timeout = ata_timeout_iters();

  ata_select_drive(dev->channel, dev->drive);

  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status == 0)
    return ATA_ERR_NODEV;

  int rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  outb(io + ATA_REG_SECCOUNT, 0);
  outb(io + ATA_REG_LBA0, 0);
  outb(io + ATA_REG_LBA1, 0);
  outb(io + ATA_REG_LBA2, 0);

  outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

  status = inb(io + ATA_REG_STATUS);
  if (status == 0)
    return ATA_ERR_NODEV;

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  uint8_t lba1 = inb(io + ATA_REG_LBA1);
  uint8_t lba2 = inb(io + ATA_REG_LBA2);

  if (lba1 != 0 || lba2 != 0) {
    dev->is_atapi = 1;
    // Drenar buffer.
    if (ata_wait_drq(io, timeout) == ATA_OK) {
      for (int i = 0; i < 256; i++)
        (void)inw(io + ATA_REG_DATA);
    }
    return ATA_ERR_NODEV;
  }

  rc = ata_wait_drq(io, timeout);
  if (rc != ATA_OK)
    return rc;

  uint16_t id[256];
  for (int i = 0; i < 256; i++)
    id[i] = inw(io + ATA_REG_DATA);

  ata_parse_identify(dev, id);
  return ATA_OK;
}

// ===========================================================================
// Selección de modo
// ===========================================================================
static int ata_select_mode(ata_device_t *dev) {
  if (dev->supports_lba48 && dev->lba48_sectors > 0) {
    dev->use_lba48 = 1;
    dev->num_sectors = dev->lba48_sectors;
  } else if (dev->supports_lba && dev->lba28_sectors > 0) {
    dev->use_lba48 = 0;
    dev->num_sectors = dev->lba28_sectors;
  } else {
    return ATA_ERR_UNKNOWN;
  }

  dev->pio_mode = 0;
  if (dev->pio_modes & (1 << 4))
    dev->pio_mode = 4;
  else if (dev->pio_modes & (1 << 3))
    dev->pio_mode = 3;
  else if (dev->pio_modes & (1 << 2))
    dev->pio_mode = 2;
  else if (dev->pio_modes & (1 << 1))
    dev->pio_mode = 1;

  uint8_t pio_val = dev->pio_mode;
  if (dev->pio_mode >= 3)
    pio_val = dev->pio_mode - 3 + 0x08;

  int rc = ata_set_features(dev->channel, dev->drive, ATA_FEATURE_TRANSFER_MODE,
                            pio_val);
  if (rc != ATA_OK) {
    dev->pio_mode = 0;
  }

  return ATA_OK;
}

// ===========================================================================
// Emisión de comandos
// ===========================================================================
static int ata_issue_cmd_lba48(ata_device_t *dev, uint64_t lba, uint32_t count,
                               uint8_t command) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

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

  outb(io + ATA_REG_COMMAND, command);
  return ATA_OK;
}

static int ata_issue_cmd_lba28(ata_device_t *dev, uint64_t lba, uint32_t count,
                               uint8_t command) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

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

  outb(io + ATA_REG_COMMAND, command);
  return ATA_OK;
}

// ===========================================================================
// Transferencia PIO
// ===========================================================================
static int ata_pio_transfer(ata_device_t *dev, uint32_t count, void *buf,
                            int dir) {
  uint16_t io = dev->channel->io_base;
  uint64_t timeout = ata_timeout_iters();
  uint16_t *ptr = (uint16_t *)buf;
  uint32_t words_per_sector = dev->sector_size / 2;

  for (uint32_t s = 0; s < count; s++) {
    int rc = ata_wait_drq(io, timeout);
    if (rc != ATA_OK)
      return rc;

    if (dir == 0) {
      for (uint32_t i = 0; i < words_per_sector; i++)
        *ptr++ = inw(io + ATA_REG_DATA);
    } else {
      for (uint32_t i = 0; i < words_per_sector; i++)
        outw(io + ATA_REG_DATA, *ptr++);
    }
  }
  return ATA_OK;
}

// ===========================================================================
// Operaciones de alto nivel
// ===========================================================================
static int ata_read(ata_device_t *dev, uint64_t lba, uint32_t count,
                    void *buf) {
  uint8_t *ptr = (uint8_t *)buf;

  if (dev->use_lba48) {
    while (count > 0) {
      uint32_t chunk = count > 65536 ? 65536 : count;
      int rc = ata_issue_cmd_lba48(dev, lba, chunk, ATA_CMD_READ_SECTORS_EXT);
      if (rc != ATA_OK)
        return rc;
      rc = ata_pio_transfer(dev, chunk, ptr, 0);
      if (rc != ATA_OK)
        return rc;
      lba += chunk;
      ptr += chunk * dev->sector_size;
      count -= chunk;
    }
  } else {
    while (count > 0) {
      uint32_t chunk = count > 255 ? 255 : count;
      int rc = ata_issue_cmd_lba28(dev, lba, chunk, ATA_CMD_READ_SECTORS);
      if (rc != ATA_OK)
        return rc;
      rc = ata_pio_transfer(dev, chunk, ptr, 0);
      if (rc != ATA_OK)
        return rc;
      lba += chunk;
      ptr += chunk * dev->sector_size;
      count -= chunk;
    }
  }
  return ATA_OK;
}

static int ata_write(ata_device_t *dev, uint64_t lba, uint32_t count,
                     const void *buf) {
  const uint8_t *ptr = (const uint8_t *)buf;

  if (dev->use_lba48) {
    while (count > 0) {
      uint32_t chunk = count > 65536 ? 65536 : count;
      int rc = ata_issue_cmd_lba48(dev, lba, chunk, ATA_CMD_WRITE_SECTORS_EXT);
      if (rc != ATA_OK)
        return rc;
      rc = ata_pio_transfer(dev, chunk, (void *)ptr, 1);
      if (rc != ATA_OK)
        return rc;
      lba += chunk;
      ptr += chunk * dev->sector_size;
      count -= chunk;
    }
  } else {
    while (count > 0) {
      uint32_t chunk = count > 255 ? 255 : count;
      int rc = ata_issue_cmd_lba28(dev, lba, chunk, ATA_CMD_WRITE_SECTORS);
      if (rc != ATA_OK)
        return rc;
      rc = ata_pio_transfer(dev, chunk, (void *)ptr, 1);
      if (rc != ATA_OK)
        return rc;
      lba += chunk;
      ptr += chunk * dev->sector_size;
      count -= chunk;
    }
  }
  return ATA_OK;
}

static int ata_flush(ata_device_t *dev) {
  uint16_t io = dev->channel->io_base;
  uint64_t timeout = ata_timeout_iters();

  int rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  ata_select_drive(dev->channel, dev->drive);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  uint8_t cmd =
      dev->supports_flush ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE;
  outb(io + ATA_REG_COMMAND, cmd);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status & ATA_SR_ERR)
    return ata_decode_error(io);
  return ATA_OK;
}

// ===========================================================================
// Reintentos
// ===========================================================================
static int ata_retry_op(ata_device_t *dev,
                        int (*op)(ata_device_t *, uint64_t, uint32_t, void *),
                        uint64_t lba, uint32_t count, void *buf) {
  int last_err = ATA_ERR_UNKNOWN;

  for (int attempt = 0; attempt < ATA_MAX_RETRIES; attempt++) {
    int rc = op(dev, lba, count, buf);
    if (rc == ATA_OK)
      return ATA_OK;

    last_err = rc;
    dev->error_count++;

    if (rc == ATA_ERR_ABRT && dev->use_lba48 && dev->supports_lba &&
        dev->lba28_sectors > 0 && lba < dev->lba28_sectors) {
      dev->use_lba48 = 0;
      dev->num_sectors = dev->lba28_sectors;
      dev->retry_count++;
      continue;
    }

    ata_soft_reset(dev->channel);
    ata_select_drive(dev->channel, dev->drive);
    dev->retry_count++;
  }
  return last_err;
}

// ===========================================================================
// block_ops_t
// ===========================================================================
static int ata_submit(block_device_t *bdev, bio_t *bio) {
  ata_device_t *dev = (ata_device_t *)bdev->private_data;
  if (!dev)
    return -ENODEV;

  ata_channel_t *ch = dev->channel;
  unsigned long flags = spin_lock_irqsave(&ch->lock);
  int rc;

  switch (bio->op) {
  case BIO_READ:
    rc = ata_retry_op(dev, (void *)ata_read, bio->lba, bio->count, bio->buf);
    break;
  case BIO_WRITE:
    rc = ata_retry_op(dev, (void *)ata_write, bio->lba, bio->count, bio->buf);
    break;
  case BIO_FLUSH:
    rc = ata_flush(dev);
    break;
  default:
    rc = -EINVAL;
    break;
  }

  spin_unlock_irqrestore(&ch->lock, flags);
  bio->error = (rc == ATA_OK) ? 0 : -EIO;
  return bio->error;
}

static void ata_submit_flush(block_device_t *bdev) {
  ata_device_t *dev = (ata_device_t *)bdev->private_data;
  if (!dev)
    return;
  unsigned long flags = spin_lock_irqsave(&dev->channel->lock);
  (void)ata_flush(dev);
  spin_unlock_irqrestore(&dev->channel->lock, flags);
}

static void ata_submit_dump(block_device_t *bdev) {
  ata_device_t *dev = (ata_device_t *)bdev->private_data;
  if (!dev)
    return;
  LOG_INFO("    modelo: '%s'", dev->model);
  LOG_INFO("    serial: '%s'", dev->serial);
  LOG_INFO("    firmware: '%s'", dev->firmware);
  LOG_INFO("    sector size: %u bytes", dev->sector_size);
  LOG_INFO("    LBA28: %s (%lu sectores)", dev->supports_lba ? "sí" : "no",
           (unsigned long)dev->lba28_sectors);
  LOG_INFO("    LBA48: %s (%lu sectores)", dev->supports_lba48 ? "sí" : "no",
           (unsigned long)dev->lba48_sectors);
  LOG_INFO("    modo activo: %s", dev->use_lba48 ? "LBA48" : "LBA28");
  LOG_INFO("    PIO mode: %u", dev->pio_mode);
}

static block_ops_t ata_pio_ops = {
    .submit = ata_submit,
    .flush = ata_submit_flush,
    .dump = ata_submit_dump,
};

// ===========================================================================
// Registro
// ===========================================================================
static const char *ata_name_for(ata_channel_t *ch, uint8_t drive) {
  if (ch->io_base == ATA_PRIMARY_IO)
    return drive ? "hdb" : "hda";
  return drive ? "hdd" : "hdc";
}

static int ata_register_device(ata_device_t *dev, const char *name) {
  block_device_t *bdev = (block_device_t *)kzalloc(sizeof(block_device_t));
  if (!bdev)
    return -ENOMEM;

  size_t i = 0;
  while (name[i] && i < sizeof(bdev->name) - 1) {
    bdev->name[i] = name[i];
    i++;
  }
  bdev->name[i] = '\0';

  bdev->num_sectors = dev->num_sectors;
  bdev->sector_size = dev->sector_size;
  bdev->is_read_only = 0;
  bdev->ops = &ata_pio_ops;
  bdev->private_data = dev;

  int rc = blk_register(bdev);
  if (rc < 0) {
    kfree(bdev);
    return rc;
  }

  dev->bdev = bdev;
  return 0;
}

// ===========================================================================
// Detección
// ===========================================================================
static void ata_probe_device(ata_channel_t *ch, uint8_t drive,
                             const char *channel_name) {
  ata_device_t *dev = (ata_device_t *)kzalloc(sizeof(ata_device_t));
  if (!dev)
    return;
  dev->channel = ch;
  dev->drive = drive;

  int rc = ata_identify(dev);
  const char *drive_name = drive ? "slave" : "master";

  if (rc == ATA_OK) {
    if (ata_select_mode(dev) != ATA_OK) {
      kfree(dev);
      return;
    }

    const char *name = ata_name_for(ch, drive);
    LOG_INFO(
        "[ATA] %s en %s %s: '%s' (%lu sectores, %lu MB, %s, PIO%u)", name,
        channel_name, drive_name, dev->model, (unsigned long)dev->num_sectors,
        (unsigned long)(dev->num_sectors * dev->sector_size / (1024 * 1024)),
        dev->use_lba48 ? "LBA48" : "LBA28", dev->pio_mode);

    if (ata_register_device(dev, name) != 0) {
      kfree(dev);
      return;
    }

    if (drive == 0)
      ch->master = (void *)dev;
    else
      ch->slave = (void *)dev;

    dev->next = g_devices;
    g_devices = dev;
    g_device_count++;
  } else if (dev->is_atapi) {
    // ATAPI: lo maneja atapi.c. Guardamos el canal para que atapi_probe
    // sepa que hay un dispositivo.
    LOG_DEBUG("[ATA] dispositivo ATAPI en %s %s (lo maneja atapi)",
              channel_name, drive_name);
    kfree(dev);
  } else {
    kfree(dev);
  }
}

static void ata_init_channel(ata_channel_t *ch, uint16_t io_base,
                             uint16_t ctrl_base, uint8_t irq,
                             const char *name) {
  ch->io_base = io_base;
  ch->ctrl_base = ctrl_base;
  ch->irq = irq;
  ch->name = name;
  ch->master = NULL;
  ch->slave = NULL;
  spin_init(&ch->lock);
}

// ===========================================================================
// API pública
// ===========================================================================
ata_channel_t *ata_get_channel(int index) {
  if (index < 0 || index > 1)
    return NULL;
  return &g_channels[index];
}

int ata_pio_init(void) {
  LOG_INFO("[ATA] Iniciando detección de discos PATA...");

  ata_init_channel(&g_channels[0], ATA_PRIMARY_IO, ATA_PRIMARY_CTRL,
                   ATA_PRIMARY_IRQ, "primario");
  ata_init_channel(&g_channels[1], ATA_SECONDARY_IO, ATA_SECONDARY_CTRL,
                   ATA_SECONDARY_IRQ, "secundario");

  ata_probe_device(&g_channels[0], 0, "primario");
  ata_probe_device(&g_channels[0], 1, "primario");
  ata_probe_device(&g_channels[1], 0, "secundario");
  ata_probe_device(&g_channels[1], 1, "secundario");

  LOG_INFO("[ATA] Detección completada (%d discos)", g_device_count);
  return 0;
}

void ata_dump(void) {
  LOG_INFO("[ATA] Discos ATA detectados: %d", g_device_count);
  for (ata_device_t *d = g_devices; d; d = d->next) {
    LOG_INFO("  %s (%s %s): '%s'", d->bdev ? d->bdev->name : "?",
             d->channel->name, d->drive ? "slave" : "master", d->model);
  }
}

// ===========================================================================
// Registro del driver
// ===========================================================================
struct driver ata_pio_driver = {
    .name = "ata_pio",
    .init = ata_pio_init,
    .shutdown = NULL,
};