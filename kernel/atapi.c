// kernel/atapi.c
//
// Driver ATAPI (CD/DVD sobre PATA).
//
// Referencia: ATA/ATAPI-7, SCSI Primary Commands (SPC-3),
// SCSI Block Commands (SBC-2), SCSI MMC-6.
//
// Arquitectura:
//   atapi_init         → detección de unidades ATAPI
//   atapi_identify_packet → IDENTIFY PACKET DEVICE
//   atapi_inquiry      → INQUIRY (vendor, modelo, revisión)
//   atapi_test_unit_ready → TEST UNIT READY
//   atapi_request_sense → REQUEST SENSE
//   atapi_read_capacity → READ CAPACITY (10)
//   atapi_read         → READ (10) con reintentos
//   atapi_submit       → dispatcher desde el block layer
//
// Usa los helpers de ata_common para timing y comandos PACKET.

#include "atapi.h"
#include "ata_common.h"
#include "block.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"


// ===========================================================================
// Comandos SCSI
// ===========================================================================
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_MODE_SENSE_6 0x1A
#define SCSI_START_STOP_UNIT 0x1B
#define SCSI_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_READ_TOC 0x43
#define SCSI_GET_CONFIGURATION 0x46
#define SCSI_GET_EVENT_STATUS_NOTIFICATION 0x4A
#define SCSI_READ_12 0xA8

// Sense keys (SPC-3).
#define SENSE_NO_SENSE 0x00
#define SENSE_RECOVERED_ERROR 0x01
#define SENSE_NOT_READY 0x02
#define SENSE_MEDIUM_ERROR 0x03
#define SENSE_HARDWARE_ERROR 0x04
#define SENSE_ILLEGAL_REQUEST 0x05
#define SENSE_UNIT_ATTENTION 0x06
#define SENSE_DATA_PROTECT 0x07
#define SENSE_ABORTED_COMMAND 0x0B

// Tipos de dispositivo SCSI (SPC-3).
#define SCSI_TYPE_DISK 0x00
#define SCSI_TYPE_TAPE 0x01
#define SCSI_TYPE_CDROM 0x05
#define SCSI_TYPE_WORM 0x04
#define SCSI_TYPE_OPTICAL 0x07
#define SCSI_TYPE_MEDIUM_CHANGER 0x08

// ===========================================================================
// Estructura de una unidad ATAPI
// ===========================================================================
typedef struct atapi_device {
  // Ubicación.
  ata_channel_t *channel;
  uint8_t drive;

  // Identidad (INQUIRY).
  uint8_t scsi_type; // 0x05 = CD/DVD
  uint8_t removable; // 1 si es extraíble
  char vendor[9];    // 8 + NUL
  char model[17];    // 16 + NUL
  char revision[5];  // 4 + NUL

  // Medio actual.
  uint8_t media_present;
  uint8_t media_changed;

  // Capacidad (READ CAPACITY).
  uint32_t sector_size; // 2048 (CD) o 4096 (DVD)
  uint64_t num_sectors;

  // Último sentido (REQUEST SENSE).
  uint8_t last_sense_key;
  uint8_t last_asc;
  uint8_t last_ascq;

  // Estado.
  uint32_t error_count;
  uint32_t retry_count;

  // Enlace al block layer.
  block_device_t *bdev;

  // Lista para debug.
  struct atapi_device *next;
} atapi_device_t;

// ===========================================================================
// Estado global
// ===========================================================================
static atapi_device_t *g_devices = NULL;
static int g_device_count = 0;

// Contador global para asignar nombres sr0, sr1, sr2, ...
// Como Linux: el primer ATAPI detectado es sr0, el segundo sr1, etc.
// No importa el canal ni el drive.
static int g_next_sr_index = 0;

// ===========================================================================
// Helpers de transferencia
//
// ATAPI funciona en dos fases:
//   1. Enviar el comando PACKET (12 bytes SCSI).
//   2. Transferir datos (si los hay).
// ===========================================================================

// Envía un comando SCSI y transfiere datos (si buf != NULL).
//
// data_len: tamaño esperado de la transferencia (0 si no hay datos).
// buf:      buffer destino/origen. NULL si no hay transferencia.
// write:    0 para leer del drive, 1 para escribir al drive.
//
// Retorna ATA_OK o un código de error de ata_common.
static int atapi_send_cmd(atapi_device_t *dev, const uint8_t *cmd12,
                          uint32_t data_len, void *buf, int write) {
  uint16_t io = dev->channel->io_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  // 1. Esperar BSY=0.
  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  // 2. Seleccionar el drive.
  ata_select_drive(dev->channel, dev->drive);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  // 3. Features = 0 (PIO).
  outb(io + ATA_REG_FEATURES, 0);

  // 4. Longitud de transferencia en LBA1/LBA2 (16 bits).
  //    LBA2 = byte alto, LBA1 = byte bajo.
  //    Si data_len es 0, no hay transferencia de datos.
  uint8_t len_lo = (uint8_t)(data_len & 0xFF);
  uint8_t len_hi = (uint8_t)((data_len >> 8) & 0xFF);
  outb(io + ATA_REG_LBA0, 0);
  outb(io + ATA_REG_LBA1, len_lo);
  outb(io + ATA_REG_LBA2, len_hi);

  // 5. Enviar PACKET (0xA0).
  outb(io + ATA_REG_COMMAND, ATA_CMD_PACKET);

  // 6. Esperar DRQ=1 (el drive está listo para recibir el comando SCSI).
  rc = ata_wait_drq(io, timeout);
  if (rc != ATA_OK)
    return rc;

  // 7. Enviar los 12 bytes del comando SCSI como 6 palabras de 16 bits.
  const uint16_t *cmd16 = (const uint16_t *)cmd12;
  for (int i = 0; i < 6; i++) {
    outw(io + ATA_REG_DATA, cmd16[i]);
  }

  // 8. Transferir datos si los hay.
  if (data_len > 0 && buf != NULL) {
    uint32_t words = (data_len + 1) / 2;
    uint16_t *ptr = (uint16_t *)buf;

    for (uint32_t w = 0; w < words; w++) {
      rc = ata_wait_drq(io, timeout);
      if (rc != ATA_OK)
        return rc;

      if (write) {
        outw(io + ATA_REG_DATA, *ptr++);
      } else {
        *ptr++ = inw(io + ATA_REG_DATA);
      }
    }
  }

  // 9. Esperar BSY=0 (el drive ha terminado).
  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  // 10. Comprobar status.
  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status & ATA_SR_ERR)
    return ata_decode_error(io);
  if (status & ATA_SR_DF)
    return ATA_ERR_DF;

  return ATA_OK;
}

// ===========================================================================
// Comandos SCSI de alto nivel
// ===========================================================================

// TEST UNIT READY (0x00). Comprueba si hay medio listo.
static int atapi_test_unit_ready(atapi_device_t *dev) {
  uint8_t cmd[12] = {0};
  cmd[0] = SCSI_TEST_UNIT_READY;
  return atapi_send_cmd(dev, cmd, 0, NULL, 0);
}

// REQUEST SENSE (0x03). Lee el estado del último error.
// Rellena dev->last_sense_key, last_asc, last_ascq.
static int atapi_request_sense(atapi_device_t *dev) {
  uint8_t cmd[12] = {0};
  cmd[0] = SCSI_REQUEST_SENSE;
  cmd[4] = 18;

  uint8_t sense[18] = {0};
  int rc = atapi_send_cmd(dev, cmd, 18, sense, 0);
  if (rc != ATA_OK)
    return rc;

  uint8_t response_code = sense[0] & 0x7F;
  if (response_code != 0x70 && response_code != 0x71)
    return ATA_ERR_UNKNOWN;

  dev->last_sense_key = sense[2] & 0x0F;
  uint8_t add_len = sense[7];
  if (add_len >= 10) {
    dev->last_asc = sense[12];
    dev->last_ascq = sense[13];
  } else {
    dev->last_asc = 0;
    dev->last_ascq = 0;
  }

  return ATA_OK;
}

// INQUIRY (0x12). Obtiene vendor, modelo, revisión y tipo.
static int atapi_inquiry(atapi_device_t *dev) {
  uint8_t cmd[12] = {0};
  cmd[0] = SCSI_INQUIRY;
  cmd[4] = 36;

  uint8_t resp[36] = {0};
  int rc = atapi_send_cmd(dev, cmd, 36, resp, 0);
  if (rc != ATA_OK)
    return rc;

  dev->scsi_type = resp[0] & 0x1F;
  dev->removable = (resp[1] & 0x80) ? 1 : 0;

  for (int i = 0; i < 8; i++)
    dev->vendor[i] = resp[8 + i];
  dev->vendor[8] = '\0';
  for (int i = 7; i >= 0 && dev->vendor[i] == ' '; i--)
    dev->vendor[i] = '\0';

  for (int i = 0; i < 16; i++)
    dev->model[i] = resp[16 + i];
  dev->model[16] = '\0';
  for (int i = 15; i >= 0 && dev->model[i] == ' '; i--)
    dev->model[i] = '\0';

  for (int i = 0; i < 4; i++)
    dev->revision[i] = resp[32 + i];
  dev->revision[4] = '\0';
  for (int i = 3; i >= 0 && dev->revision[i] == ' '; i--)
    dev->revision[i] = '\0';

  return ATA_OK;
}

// READ CAPACITY (10) (0x25). Obtiene el último LBA y el sector size.
static int atapi_read_capacity(atapi_device_t *dev) {
  uint8_t cmd[12] = {0};
  cmd[0] = SCSI_READ_CAPACITY_10;

  uint8_t resp[8] = {0};
  int rc = atapi_send_cmd(dev, cmd, 8, resp, 0);
  if (rc != ATA_OK)
    return rc;

  uint32_t last_lba = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                      ((uint32_t)resp[2] << 8) | (uint32_t)resp[3];
  uint32_t block_size = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                        ((uint32_t)resp[6] << 8) | (uint32_t)resp[7];

  if (block_size == 0)
    block_size = 2048;

  dev->sector_size = block_size;
  dev->num_sectors = (uint64_t)last_lba + 1;

  return ATA_OK;
}

// READ (10) (0x28). Lee `count` bloques desde `lba`.
static int atapi_read_10(atapi_device_t *dev, uint64_t lba, uint32_t count,
                         void *buf) {
  if (lba > 0xFFFFFFFFULL)
    return ATA_ERR_UNKNOWN;
  if (count > 0xFFFF)
    return ATA_ERR_UNKNOWN;

  uint8_t cmd[12] = {0};
  cmd[0] = SCSI_READ_10;
  cmd[2] = (uint8_t)((lba >> 24) & 0xFF);
  cmd[3] = (uint8_t)((lba >> 16) & 0xFF);
  cmd[4] = (uint8_t)((lba >> 8) & 0xFF);
  cmd[5] = (uint8_t)(lba & 0xFF);
  cmd[7] = (uint8_t)((count >> 8) & 0xFF);
  cmd[8] = (uint8_t)(count & 0xFF);

  uint32_t data_len = count * dev->sector_size;
  return atapi_send_cmd(dev, cmd, data_len, buf, 0);
}

// ===========================================================================
// Detección de medio
// ===========================================================================

static int atapi_check_media(atapi_device_t *dev) {
  int rc = atapi_test_unit_ready(dev);
  if (rc == ATA_OK) {
    if (!dev->media_present) {
      dev->media_changed = 1;
    }
    dev->media_present = 1;
    return ATA_OK;
  }

  int sense_rc = atapi_request_sense(dev);
  if (sense_rc != ATA_OK) {
    dev->media_present = 0;
    return rc;
  }

  if (dev->last_sense_key == SENSE_NOT_READY) {
    dev->media_present = 0;
    return ATA_ERR_NODEV;
  }

  if (dev->last_sense_key == SENSE_UNIT_ATTENTION) {
    dev->media_changed = 1;
    return atapi_test_unit_ready(dev);
  }

  return rc;
}

static int atapi_detect_media(atapi_device_t *dev) {
  dev->media_present = 0;
  dev->media_changed = 0;
  dev->num_sectors = 0;

  int rc = atapi_check_media(dev);
  if (rc != ATA_OK) {
    dev->sector_size = 2048;
    return ATA_OK;
  }

  rc = atapi_read_capacity(dev);
  if (rc != ATA_OK) {
    LOG_WARN("[ATAPI] READ CAPACITY falló (rc=%d), sense=0x%02x/0x%02x", rc,
             dev->last_sense_key, dev->last_asc);
    atapi_request_sense(dev);
    dev->sector_size = 2048;
    dev->num_sectors = 0;
    return ATA_OK;
  }

  return ATA_OK;
}

// ===========================================================================
// IDENTIFY PACKET DEVICE (detección ATAPI)
//
// Estrategia (basada en libata de Linux, ata_dev_classify):
//
//   1. Seleccionar el drive.
//   2. Si status == 0x00 → no hay dispositivo.
//   3. Esperar BSY=0.
//   4. Resetear los registros Sector Count, LBA0, LBA1, LBA2.
//   5. Enviar IDENTIFY PACKET DEVICE (0xA1).
//   6. Leer status.
//   7. Si status == 0x00 → no hay dispositivo.
//   8. Clasificar el dispositivo con lógica híbrida:
//
//      a. Leer LBA1 (Mid) y LBA2 (High) inmediatamente.
//      b. Si LBA1 == 0x14 && LBA2 == 0xEB → es ATAPI (firma estándar).
//      c. Si NO hay firma pero status tiene DRQ=1 y ERR=0 → es ATAPI.
//      d. Si status tiene ERR=1 → no es ATAPI (probablemente ATA).
//      e. Si status == 0x00 → no hay dispositivo.
//
//   9. Si es ATAPI: esperar DRQ=1 y drenar el buffer del IDENTIFY.
//
// Referencia: Linux drivers/ata/libata-core.c, ata_dev_classify().
// ===========================================================================
static int atapi_identify_packet(atapi_device_t *dev) {
  uint16_t io = dev->channel->io_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  LOG_DEBUG("[ATAPI] identify_packet: io=0x%x drive=%u", io, dev->drive);

  // 1. Seleccionar el drive.
  ata_select_drive(dev->channel, dev->drive);

  // 2. Leer status.
  uint8_t status = inb(io + ATA_REG_STATUS);
  LOG_DEBUG("[ATAPI]   status tras select = 0x%02x", status);
  if (status == 0)
    return ATA_ERR_NODEV;

  // 3. Esperar BSY=0.
  rc = ata_wait_not_busy(io, timeout);
  LOG_DEBUG("[ATAPI]   wait_not_busy (1) = %d", rc);
  if (rc != ATA_OK)
    return rc;

  // 4. Resetear registros.
  outb(io + ATA_REG_SECCOUNT, 0);
  outb(io + ATA_REG_LBA0, 0);
  outb(io + ATA_REG_LBA1, 0);
  outb(io + ATA_REG_LBA2, 0);

  // 5. Enviar IDENTIFY PACKET DEVICE.
  outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);

  // 6. Leer status.
  status = inb(io + ATA_REG_STATUS);
  LOG_DEBUG("[ATAPI]   status tras IDENTIFY_PACKET = 0x%02x", status);
  if (status == 0)
    return ATA_ERR_NODEV;

  // 7. Leer LBA1/LBA2 INMEDIATAMENTE.
  uint8_t lba1 = inb(io + ATA_REG_LBA1);
  uint8_t lba2 = inb(io + ATA_REG_LBA2);
  LOG_DEBUG("[ATAPI]   lba1=0x%02x lba2=0x%02x", lba1, lba2);

  // 8. Clasificar el dispositivo (lógica híbrida, como Linux).
  int is_atapi = 0;

  if (lba1 == 0x14 && lba2 == 0xEB) {
    LOG_DEBUG("[ATAPI]   firma ATAPI detectada (0x14/0xEB)");
    is_atapi = 1;
  } else if (!(status & ATA_SR_ERR) && (status & ATA_SR_DRQ)) {
    LOG_DEBUG("[ATAPI]   sin firma pero status coherente con ATAPI "
              "(DRQ=1, ERR=0)");
    is_atapi = 1;
  } else if (status & ATA_SR_ERR) {
    LOG_DEBUG("[ATAPI]   ERR=1, no es ATAPI (probablemente ATA)");
    return ATA_ERR_NODEV;
  } else {
    LOG_DEBUG("[ATAPI]   estado inesperado (status=0x%02x), no es ATAPI",
              status);
    return ATA_ERR_NODEV;
  }

  if (!is_atapi)
    return ATA_ERR_NODEV;

  // 9. Esperar BSY=0 y DRQ=1.
  rc = ata_wait_not_busy(io, timeout);
  LOG_DEBUG("[ATAPI]   wait_not_busy (2) = %d", rc);
  if (rc != ATA_OK)
    return rc;

  rc = ata_wait_drq(io, timeout);
  LOG_DEBUG("[ATAPI]   wait_drq = %d", rc);
  if (rc != ATA_OK)
    return rc;

  // 10. Drenar los 256 words (512 bytes) del buffer del IDENTIFY.
  for (int i = 0; i < 256; i++)
    (void)inw(io + ATA_REG_DATA);

  LOG_DEBUG("[ATAPI]   identify_packet OK");
  return ATA_OK;
}

// ===========================================================================
// Lectura con reintentos
// ===========================================================================
static int atapi_read_with_retry(atapi_device_t *dev, uint64_t lba,
                                 uint32_t count, void *buf) {
  int last_err = ATA_ERR_UNKNOWN;

  for (int attempt = 0; attempt < ATA_MAX_RETRIES; attempt++) {
    int rc = atapi_check_media(dev);
    if (rc != ATA_OK) {
      return rc;
    }

    rc = atapi_read_10(dev, lba, count, buf);
    if (rc == ATA_OK)
      return ATA_OK;

    last_err = rc;
    dev->error_count++;

    if (atapi_request_sense(dev) == ATA_OK) {
      if (dev->last_sense_key == SENSE_UNIT_ATTENTION) {
        dev->media_changed = 1;
        atapi_detect_media(dev);
        dev->retry_count++;
        continue;
      }
      if (dev->last_sense_key == SENSE_NOT_READY) {
        dev->media_present = 0;
        return ATA_ERR_NODEV;
      }
    }

    ata_soft_reset(dev->channel);
    dev->retry_count++;
  }

  return last_err;
}

// ===========================================================================
// block_ops_t
// ===========================================================================
static int atapi_submit(block_device_t *bdev, bio_t *bio) {
  atapi_device_t *dev = (atapi_device_t *)bdev->private_data;
  if (!dev)
    return -ENODEV;

  ata_channel_t *ch = dev->channel;
  unsigned long flags = spin_lock_irqsave(&ch->lock);
  int rc;

  switch (bio->op) {
  case BIO_READ: {
    if (bio->count == 0 || bio->lba >= dev->num_sectors ||
        bio->count > dev->num_sectors - bio->lba) {
      rc = ATA_ERR_IDNF;
      break;
    }
    uint8_t *ptr = (uint8_t *)bio->buf;
    uint64_t lba = bio->lba;
    uint32_t remaining = bio->count;
    while (remaining > 0) {
      uint32_t chunk = remaining > 0xFFFF ? 0xFFFF : remaining;
      rc = atapi_read_with_retry(dev, lba, chunk, ptr);
      if (rc != ATA_OK)
        break;
      lba += chunk;
      ptr += chunk * dev->sector_size;
      remaining -= chunk;
    }
    break;
  }
  case BIO_WRITE:
    rc = -EROFS;
    break;
  case BIO_FLUSH:
    rc = ATA_OK;
    break;
  default:
    rc = -EINVAL;
    break;
  }

  spin_unlock_irqrestore(&ch->lock, flags);

  if (rc == -EROFS || rc == -EINVAL)
    bio->error = rc;
  else
    bio->error = (rc == ATA_OK) ? 0 : -EIO;
  return bio->error;
}

static void atapi_submit_dump(block_device_t *bdev) {
  atapi_device_t *dev = (atapi_device_t *)bdev->private_data;
  if (!dev)
    return;
  LOG_INFO("    vendor: '%s'", dev->vendor);
  LOG_INFO("    modelo: '%s'", dev->model);
  LOG_INFO("    revision: '%s'", dev->revision);
  LOG_INFO("    tipo SCSI: 0x%02x", dev->scsi_type);
  LOG_INFO("    extraíble: %s", dev->removable ? "sí" : "no");
  LOG_INFO("    medio presente: %s", dev->media_present ? "sí" : "no");
  LOG_INFO("    sector size: %u bytes", dev->sector_size);
  LOG_INFO(
      "    capacidad: %lu sectores (%lu MB)", (unsigned long)dev->num_sectors,
      (unsigned long)(dev->num_sectors * dev->sector_size / (1024 * 1024)));
}

static block_ops_t atapi_ops = {
    .submit = atapi_submit,
    .flush = NULL,
    .dump = atapi_submit_dump,
};

// ===========================================================================
// Nombre del dispositivo
//
// Como Linux: contador global. El primer ATAPI detectado es sr0, el
// segundo sr1, etc. No importa el canal ni el drive.
// ===========================================================================
static void atapi_assign_name(char *out) {
  int idx = g_next_sr_index++;
  // Formato: sr0, sr1, sr2, ..., sr99.
  if (idx < 10) {
    out[0] = 's';
    out[1] = 'r';
    out[2] = (char)('0' + idx);
    out[3] = '\0';
  } else if (idx < 100) {
    out[0] = 's';
    out[1] = 'r';
    out[2] = (char)('0' + (idx / 10));
    out[3] = (char)('0' + (idx % 10));
    out[4] = '\0';
  } else {
    // Fallback raro.
    out[0] = 's';
    out[1] = 'r';
    out[2] = '?';
    out[3] = '\0';
  }
}

// ===========================================================================
// Registro
// ===========================================================================
static int atapi_register_device(atapi_device_t *dev, const char *name) {
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
  bdev->sector_size = dev->sector_size ? dev->sector_size : 2048;
  bdev->is_read_only = 1;
  bdev->ops = &atapi_ops;
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
static void atapi_probe_device(ata_channel_t *ch, uint8_t drive,
                               const char *channel_name) {
  atapi_device_t *dev = (atapi_device_t *)kzalloc(sizeof(atapi_device_t));
  if (!dev)
    return;
  dev->channel = ch;
  dev->drive = drive;
  dev->sector_size = 2048; // default para CD

  // Reset del canal ANTES de IDENTIFY PACKET.
  // El IDENTIFY DEVICE previo (de ata_pio_init) pudo dejar ERR=1
  // en el drive, y eso interfiere con la detección de ATAPI.
  ata_soft_reset(ch);

  int rc = atapi_identify_packet(dev);
  const char *drive_name = drive ? "slave" : "master";

  if (rc != ATA_OK) {
    kfree(dev);
    return;
  }

  // Es ATAPI. INQUIRY para identidad.
  rc = atapi_inquiry(dev);
  if (rc != ATA_OK) {
    LOG_WARN("[ATAPI] INQUIRY falló en %s %s (rc=%d)", channel_name, drive_name,
             rc);
    // Continuar igualmente, con identidad vacía.
  }

  // Comprobar tipo SCSI.
  if (dev->scsi_type != SCSI_TYPE_CDROM && dev->scsi_type != SCSI_TYPE_WORM &&
      dev->scsi_type != SCSI_TYPE_OPTICAL &&
      dev->scsi_type != SCSI_TYPE_MEDIUM_CHANGER) {
    LOG_INFO("[ATAPI] tipo 0x%02x no soportado en %s %s, ignorando",
             dev->scsi_type, channel_name, drive_name);
    kfree(dev);
    return;
  }

  // Detectar medio y capacidad.
  atapi_detect_media(dev);

  // Asignar nombre global (sr0, sr1, ...).
  char name[16];
  atapi_assign_name(name);

  LOG_INFO("[ATAPI] %s en %s %s: '%s %s' (%lu sectores de %u bytes, "
           "medio=%s)",
           name, channel_name, drive_name, dev->vendor, dev->model,
           (unsigned long)dev->num_sectors, dev->sector_size,
           dev->media_present ? "sí" : "no");

  if (atapi_register_device(dev, name) != 0) {
    kfree(dev);
    return;
  }

  dev->next = g_devices;
  g_devices = dev;
  g_device_count++;
}

// ===========================================================================
// API pública
// ===========================================================================
int atapi_init(void) {
  LOG_INFO("[ATAPI] Iniciando detección de unidades ATAPI...");

  // Los canales ya están inicializados por ata_pio_init.
  extern ata_channel_t *ata_get_channel(int index);
  ata_channel_t *prim = ata_get_channel(0);
  ata_channel_t *sec = ata_get_channel(1);

  if (prim) {
    atapi_probe_device(prim, 0, "primario");
    atapi_probe_device(prim, 1, "primario");
  }
  if (sec) {
    atapi_probe_device(sec, 0, "secundario");
    atapi_probe_device(sec, 1, "secundario");
  }

  LOG_INFO("[ATAPI] Detección completada (%d unidades)", g_device_count);
  return 0;
}

void atapi_dump(void) {
  LOG_INFO("[ATAPI] Unidades detectadas: %d", g_device_count);
  for (atapi_device_t *d = g_devices; d; d = d->next) {
    LOG_INFO("  %s (%s %s): '%s %s'", d->bdev ? d->bdev->name : "?",
             d->channel->name, d->drive ? "slave" : "master", d->vendor,
             d->model);
  }
}

// ===========================================================================
// Registro del driver
// ===========================================================================
struct driver atapi_driver = {
    .name = "atapi",
    .init = atapi_init,
    .shutdown = NULL,
};