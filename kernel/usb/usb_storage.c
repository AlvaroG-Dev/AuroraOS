// kernel/usb/usb_storage.c
//
// Driver USB Mass Storage (Bulk-Only Transport + SCSI transparent
// command set). Se registra como block_device_t y el block layer lo
// trata exactamente igual que un disco AHCI.
//
// Referencias:
//   - USB Mass Storage Class Bulk-Only Transport 1.0 (usbmassbulk_10)
//   - SCSI Primary Commands (SPC-3): INQUIRY, REQUEST_SENSE, TEST_UNIT_READY
//   - SCSI Block Commands (SBC-2): READ_CAPACITY(10), READ(10), WRITE(10),
//                                  SYNCHRONIZE_CACHE(10)
//
// Modelo de ejecución: submit() síncrono. Como el bloque layer usa
// bdev_submit_sync() para los wrappers (bdev_read/bdev_write), el
// kthread no se bloquea con spinlocks; cada bio hace sus bulk transfers
// y llama bio_endio() al final.

#include "usb_storage.h"
#include "../block.h"
#include "../heap.h"
#include "../klog.h"
#include "../string.h"
#include "../uaccess.h"
#include "usb_dma.h"

// ===========================================================================
// Protocolo BOT
// ===========================================================================
typedef struct __attribute__((packed)) {
  uint32_t signature;            // 0x43425355 "USBC"
  uint32_t tag;                  // matching con CSW
  uint32_t data_transfer_length; // bytes de la fase de datos
  uint8_t flags;                 // bit 7: 1=IN (device->host), 0=OUT
  uint8_t lun;
  uint8_t cb_length; // 1..16
  uint8_t cb[16];    // SCSI Command Block
} usb_bot_cbw_t;

typedef struct __attribute__((packed)) {
  uint32_t signature; // 0x53425355 "USBS"
  uint32_t tag;
  uint32_t data_residue;
  uint8_t status; // 0=OK, 1=Failed, 2=Phase Error
} usb_bot_csw_t;

#define BOT_CBW_SIG 0x43425355u
#define BOT_CSW_SIG 0x53425355u

// ===========================================================================
// Opcodes SCSI
// ===========================================================================
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_MODE_SENSE_6 0x1A
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2A
#define SCSI_SYNCHRONIZE_CACHE_10 0x35

// ===========================================================================
// Periferal Device Types (byte 0 de INQUIRY, low 5 bits)
// ===========================================================================
#define SCSI_TYPE_DISK 0x00    // Direct Access Block Device
#define SCSI_TYPE_CDROM 0x05   // CD-ROM / DVD-ROM
#define SCSI_TYPE_OPTICAL 0x07 // Optical Memory (MO)
#define SCSI_TYPE_RBC 0x0E     // Reduced Block Commands (flash)

// ===========================================================================
// Límites
// ===========================================================================
#define USB_STORAGE_MAX_XFER (128 * 1024) // buffer DMA reutilizable
#define USB_STORAGE_TIMEOUT_MS 5000

// ===========================================================================
// Estado por dispositivo de storage
// ===========================================================================
typedef struct {
  usb_device_t *dev;
  usb_endpoint_t *bulk_in;
  usb_endpoint_t *bulk_out;
  uint32_t next_tag;
  uint8_t lun;
  uint8_t dev_type; // byte 0 de INQUIRY (Peripheral Device Type)

  uint32_t num_sectors;
  uint32_t sector_size;

  char name[8];
  block_device_t *bdev;

  usb_dma_buf_t cbw_buf;
  usb_dma_buf_t csw_buf;
  usb_dma_buf_t data_buf;

  char inquiry_vendor[9];
  char inquiry_product[17];
  char inquiry_revision[5];
} usb_storage_t;

// ===========================================================================
// Nombres "sdX" (discos) y "srX" (CD/DVD). Busca el siguiente libre.
// ===========================================================================
static void usb_storage_assign_name(const char *prefix, char *out) {
  static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
  char name[8];
  for (int i = 0; i < 26; i++) {
    name[0] = prefix[0];
    name[1] = prefix[1];
    name[2] = letters[i];
    name[3] = '\0';
    if (!blk_lookup(name)) {
      strcpy(out, name);
      return;
    }
  }
  strcpy(out, prefix);
}

// ===========================================================================
// Una transacción BOT: CBW → data (opcional) → CSW.
// ===========================================================================
static int usb_storage_bot_xfer(usb_storage_t *s, const uint8_t *cb, int cb_len,
                                void *data, uint32_t data_len, int dir_in,
                                uint32_t timeout_ms) {
  if (!s || !cb || cb_len < 1 || cb_len > 16)
    return -EINVAL;
  if (data_len > s->data_buf.size)
    return -EINVAL;

  // --- 1. CBW -----------------------------------------------------------
  usb_bot_cbw_t *cbw = (usb_bot_cbw_t *)s->cbw_buf.virt;
  uint32_t tag = ++s->next_tag;

  memset(cbw, 0, sizeof(*cbw));
  cbw->signature = BOT_CBW_SIG;
  cbw->tag = tag;
  cbw->data_transfer_length = data_len;
  cbw->flags = dir_in ? 0x80 : 0x00;
  cbw->lun = s->lun;
  cbw->cb_length = (uint8_t)cb_len;
  memcpy(cbw->cb, cb, cb_len);
  usb_dma_sync(&s->cbw_buf, USB_DMA_TO_DEVICE);

  int rc = s->dev->hcd->bulk_xfer(s->dev, s->bulk_out, s->cbw_buf.virt,
                                  sizeof(*cbw), 0, timeout_ms);
  if (rc < 0)
    return rc;
  if (rc != (int)sizeof(*cbw)) {
    LOG_ERR("[USB-STG] %s: CBW short (rc=%d)", s->name, rc);
    return -EIO;
  }

  // --- 2. Data stage (si data_len > 0) ---------------------------------
  if (data && data_len > 0) {
    if (dir_in) {
      rc = s->dev->hcd->bulk_xfer(s->dev, s->bulk_in, s->data_buf.virt,
                                  (int)data_len, 1, timeout_ms);
      if (rc < 0)
        return rc;
      if (rc != (int)data_len) {
        LOG_ERR("[USB-STG] %s: data-in short (rc=%d, want=%u)", s->name, rc,
                data_len);
        return -EIO;
      }
      usb_dma_sync(&s->data_buf, USB_DMA_FROM_DEVICE);
      memcpy(data, s->data_buf.virt, data_len);
    } else {
      memcpy(s->data_buf.virt, data, data_len);
      usb_dma_sync(&s->data_buf, USB_DMA_TO_DEVICE);
      rc = s->dev->hcd->bulk_xfer(s->dev, s->bulk_out, s->data_buf.virt,
                                  (int)data_len, 0, timeout_ms);
      if (rc < 0)
        return rc;
      if (rc != (int)data_len) {
        LOG_ERR("[USB-STG] %s: data-out short (rc=%d, want=%u)", s->name, rc,
                data_len);
        return -EIO;
      }
    }
  }

  // --- 3. CSW -----------------------------------------------------------
  memset(s->csw_buf.virt, 0, sizeof(usb_bot_csw_t));
  rc = s->dev->hcd->bulk_xfer(s->dev, s->bulk_in, s->csw_buf.virt,
                              sizeof(usb_bot_csw_t), 1, timeout_ms);
  if (rc < 0)
    return rc;
  if (rc != (int)sizeof(usb_bot_csw_t)) {
    LOG_ERR("[USB-STG] %s: CSW short (rc=%d)", s->name, rc);
    return -EIO;
  }
  usb_dma_sync(&s->csw_buf, USB_DMA_FROM_DEVICE);

  usb_bot_csw_t *csw = (usb_bot_csw_t *)s->csw_buf.virt;
  if (csw->signature != BOT_CSW_SIG) {
    LOG_ERR("[USB-STG] %s: CSW bad signature 0x%08x", s->name, csw->signature);
    return -EIO;
  }
  if (csw->tag != tag) {
    LOG_ERR("[USB-STG] %s: CSW tag mismatch (%u != %u)", s->name, csw->tag,
            tag);
    return -EIO;
  }
  if (csw->status != 0) {
    LOG_ERR("[USB-STG] %s: CSW status=%u residue=%u", s->name, csw->status,
            csw->data_residue);
    return -EIO;
  }
  return 0;
}

// ===========================================================================
// Comandos SCSI
// ===========================================================================
static int scsi_test_unit_ready(usb_storage_t *s) {
  uint8_t cb[6] = {SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0};
  return usb_storage_bot_xfer(s, cb, 6, NULL, 0, 0, USB_STORAGE_TIMEOUT_MS);
}

static int scsi_request_sense(usb_storage_t *s, uint8_t *sense,
                              uint32_t sense_len) {
  uint8_t cb[6] = {SCSI_REQUEST_SENSE,
                   0,
                   0,
                   0,
                   (uint8_t)(sense_len > 0xFF ? 0xFF : sense_len),
                   0};
  return usb_storage_bot_xfer(s, cb, 6, sense, sense_len, 1,
                              USB_STORAGE_TIMEOUT_MS);
}

static int scsi_inquiry(usb_storage_t *s) {
  uint8_t buf[36];
  memset(buf, 0, sizeof(buf));
  uint8_t cb[6] = {SCSI_INQUIRY, 0, 0, 0, sizeof(buf), 0};
  int rc = usb_storage_bot_xfer(s, cb, 6, buf, sizeof(buf), 1,
                                USB_STORAGE_TIMEOUT_MS);
  if (rc != 0)
    return rc;

  // Byte 0, bits 4:0 = Peripheral Device Type (bits 6:5 reservados,
  // bit 7 = RMB / removable media bit).
  s->dev_type = buf[0] & 0x1F;

  memcpy(s->inquiry_vendor, buf + 8, 8);
  s->inquiry_vendor[8] = '\0';
  memcpy(s->inquiry_product, buf + 16, 16);
  s->inquiry_product[16] = '\0';
  memcpy(s->inquiry_revision, buf + 32, 4);
  s->inquiry_revision[4] = '\0';

  // Trim trailing spaces (formato SCSI: campo padded con 0x20)
  for (int i = 7; i >= 0 && s->inquiry_vendor[i] == ' '; i--)
    s->inquiry_vendor[i] = '\0';
  for (int i = 15; i >= 0 && s->inquiry_product[i] == ' '; i--)
    s->inquiry_product[i] = '\0';
  for (int i = 3; i >= 0 && s->inquiry_revision[i] == ' '; i--)
    s->inquiry_revision[i] = '\0';

  LOG_INFO("[USB-STG] %s: type=0x%02x '%s' '%s' rev '%s'", s->name, s->dev_type,
           s->inquiry_vendor, s->inquiry_product, s->inquiry_revision);
  return 0;
}

static int scsi_read_capacity_10(usb_storage_t *s, uint32_t *last_lba,
                                 uint32_t *block_size) {
  uint8_t buf[8];
  memset(buf, 0, sizeof(buf));
  uint8_t cb[10] = {SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  int rc = usb_storage_bot_xfer(s, cb, 10, buf, sizeof(buf), 1,
                                USB_STORAGE_TIMEOUT_MS);
  if (rc != 0)
    return rc;

  *last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
              ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
  *block_size = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
  return 0;
}

static int scsi_read_10(usb_storage_t *s, uint64_t lba, uint32_t count,
                        void *buf) {
  if (count == 0 || count > 0xFFFF)
    return -EINVAL;
  uint8_t cb[10];
  cb[0] = SCSI_READ_10;
  cb[1] = 0;
  cb[2] = (lba >> 24) & 0xFF;
  cb[3] = (lba >> 16) & 0xFF;
  cb[4] = (lba >> 8) & 0xFF;
  cb[5] = (lba) & 0xFF;
  cb[6] = 0;
  cb[7] = (count >> 8) & 0xFF;
  cb[8] = (count) & 0xFF;
  cb[9] = 0;
  uint32_t bytes = count * s->sector_size;
  return usb_storage_bot_xfer(s, cb, 10, buf, bytes, 1, USB_STORAGE_TIMEOUT_MS);
}

static int scsi_write_10(usb_storage_t *s, uint64_t lba, uint32_t count,
                         const void *buf) {
  if (count == 0 || count > 0xFFFF)
    return -EINVAL;
  uint8_t cb[10];
  cb[0] = SCSI_WRITE_10;
  cb[1] = 0;
  cb[2] = (lba >> 24) & 0xFF;
  cb[3] = (lba >> 16) & 0xFF;
  cb[4] = (lba >> 8) & 0xFF;
  cb[5] = (lba) & 0xFF;
  cb[6] = 0;
  cb[7] = (count >> 8) & 0xFF;
  cb[8] = (count) & 0xFF;
  cb[9] = 0;
  uint32_t bytes = count * s->sector_size;
  return usb_storage_bot_xfer(s, cb, 10, (void *)buf, bytes, 0,
                              USB_STORAGE_TIMEOUT_MS);
}

static int scsi_sync_cache(usb_storage_t *s) {
  uint8_t cb[10] = {SCSI_SYNCHRONIZE_CACHE_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  return usb_storage_bot_xfer(s, cb, 10, NULL, 0, 0, USB_STORAGE_TIMEOUT_MS);
}

// ===========================================================================
// Helpers de transferencia con troceado (bio puede ser mayor que
// nuestro data_buf de 128 KB).
// ===========================================================================
static int usb_storage_do_read(usb_storage_t *s, uint64_t lba, uint32_t count,
                               void *buf) {
  uint8_t *p = (uint8_t *)buf;
  uint32_t max_blocks = (uint32_t)(s->data_buf.size / s->sector_size);
  if (max_blocks == 0 || max_blocks > 0xFFFF)
    max_blocks = 0xFFFF;

  while (count > 0) {
    uint32_t n = (count < max_blocks) ? count : max_blocks;
    int rc = scsi_read_10(s, lba, n, p);
    if (rc != 0)
      return rc;
    p += (uint64_t)n * s->sector_size;
    lba += n;
    count -= n;
  }
  return 0;
}

static int usb_storage_do_write(usb_storage_t *s, uint64_t lba, uint32_t count,
                                const void *buf) {
  const uint8_t *p = (const uint8_t *)buf;
  uint32_t max_blocks = (uint32_t)(s->data_buf.size / s->sector_size);
  if (max_blocks == 0 || max_blocks > 0xFFFF)
    max_blocks = 0xFFFF;

  while (count > 0) {
    uint32_t n = (count < max_blocks) ? count : max_blocks;
    int rc = scsi_write_10(s, lba, n, p);
    if (rc != 0)
      return rc;
    p += (uint64_t)n * s->sector_size;
    lba += n;
    count -= n;
  }

  // Aseguramos que el device persiste los datos. Si el device no
  // soporta SYNCHRONIZE CACHE, BOT devolverá un CSW con status
  // != 0 y lo ignoramos aquí (el write en sí ya tuvo éxito).
  (void)scsi_sync_cache(s);
  return 0;
}

// ===========================================================================
// block_ops_t
// ===========================================================================
static int usb_storage_submit(block_device_t *bdev, bio_t *bio) {
  usb_storage_t *s = (usb_storage_t *)bdev->private_data;
  if (!s) {
    bio_endio(bio, -ENODEV);
    return 0;
  }

  int rc = 0;
  switch (bio->op) {
  case BIO_READ:
    rc = usb_storage_do_read(s, bio->lba, bio->count, bio->buf);
    break;
  case BIO_WRITE:
    rc = usb_storage_do_write(s, bio->lba, bio->count, bio->buf);
    break;
  case BIO_FLUSH:
    rc = scsi_sync_cache(s);
    break;
  default:
    rc = -EINVAL;
    break;
  }

  bio_endio(bio, rc);
  return 0;
}

static void usb_storage_flush_marker(block_device_t *bdev) {
  // Basta con que no sea NULL: el block layer lo usa como gate para
  // enviar el BIO_FLUSH a submit(). La implementación real está ahí.
  (void)bdev;
}

static void usb_storage_dump(block_device_t *bdev) {
  usb_storage_t *s = (usb_storage_t *)bdev->private_data;
  if (!s)
    return;
  LOG_INFO("    USB-STG %s: '%s %s' %u B/sector, %u sectores%s", s->name,
           s->inquiry_vendor, s->inquiry_product, s->sector_size,
           s->num_sectors, bdev->is_read_only ? " [RO]" : "");
}

static block_ops_t usb_storage_ops = {
    .submit = usb_storage_submit,
    .flush = usb_storage_flush_marker,
    .dump = usb_storage_dump,
};

// ===========================================================================
// Probe
// ===========================================================================
int usb_storage_probe(usb_device_t *dev) {
  if (!dev || !dev->hcd || !dev->hcd->bulk_xfer || !dev->hcd->configure_ep)
    return -EINVAL;

  usb_config_t *cfg = dev->configs[0];
  if (!cfg)
    return -EINVAL;

  for (int i = 0; i < cfg->num_interface_structs; i++) {
    usb_interface_t *itf = &cfg->interfaces[i];

    // USB Mass Storage Class (0x08) / SCSI transparent (0x06) /
    // Bulk-Only Transport (0x50).
    if (itf->class != 0x08 || itf->subclass != 0x06 || itf->protocol != 0x50)
      continue;

    // Buscar los dos bulk endpoints (uno IN, uno OUT).
    usb_endpoint_t *bulk_in = NULL;
    usb_endpoint_t *bulk_out = NULL;
    for (int j = 0; j < itf->num_endpoints; j++) {
      usb_endpoint_t *ep = &itf->endpoints[j];
      if (ep->type != USB_EP_BULK)
        continue;
      if (ep->addr & 0x80)
        bulk_in = ep;
      else
        bulk_out = ep;
    }
    if (!bulk_in || !bulk_out) {
      LOG_ERR("[USB-STG] iface %u: falta bulk IN/OUT", itf->number);
      return -ENODEV;
    }

    LOG_INFO("[USB-STG] iface %u BOT (EP%u IN, EP%u OUT)", itf->number,
             bulk_in->addr & 0x0F, bulk_out->addr & 0x0F);

    // Allocar state.
    usb_storage_t *s = (usb_storage_t *)kzalloc(sizeof(*s));
    if (!s)
      return -ENOMEM;
    s->dev = dev;
    s->bulk_in = bulk_in;
    s->bulk_out = bulk_out;
    s->next_tag = 0;
    s->lun = 0;

    // Nombre temporal para los logs iniciales (aún no sabemos si es
    // disco o CD). Se reasignará tras INQUIRY.
    strcpy(s->name, "usbstg");

    // Buffers DMA.
    int rc = usb_dma_alloc(sizeof(usb_bot_cbw_t), &s->cbw_buf);
    if (rc != 0)
      goto fail_free;
    rc = usb_dma_alloc(sizeof(usb_bot_csw_t), &s->csw_buf);
    if (rc != 0)
      goto fail_free;
    rc = usb_dma_alloc(USB_STORAGE_MAX_XFER, &s->data_buf);
    if (rc != 0)
      goto fail_free;

    // Configure Endpoint para ambos bulk.
    rc = dev->hcd->configure_ep(dev, bulk_in);
    if (rc != 0) {
      LOG_ERR("[USB-STG] configure_ep(bulk_in) falló: %d", rc);
      goto fail_free;
    }
    rc = dev->hcd->configure_ep(dev, bulk_out);
    if (rc != 0) {
      LOG_ERR("[USB-STG] configure_ep(bulk_out) falló: %d", rc);
      goto fail_free;
    }

    // SCSI init: TEST_UNIT_READY con retry de REQUEST_SENSE (a veces
    // el primer comando devuelve CHECK CONDITION "unit attention").
    rc = scsi_test_unit_ready(s);
    for (int r = 0; r < 3 && rc != 0; r++) {
      uint8_t sense[18];
      (void)scsi_request_sense(s, sense, sizeof(sense));
      rc = scsi_test_unit_ready(s);
    }
    if (rc != 0) {
      LOG_ERR("[USB-STG] %s: TEST_UNIT_READY falló tras reintentos: %d",
              s->name, rc);
      goto fail_free;
    }

    // INQUIRY: obtenemos tipo, vendor, product, revision.
    rc = scsi_inquiry(s);
    if (rc != 0) {
      LOG_ERR("[USB-STG] %s: INQUIRY falló: %d", s->name, rc);
      goto fail_free;
    }

    // Ahora sí sabemos si es CD-ROM, disco, óptico, RBC...
    int is_cdrom =
        (s->dev_type == SCSI_TYPE_CDROM || s->dev_type == SCSI_TYPE_OPTICAL);

    // Reasignar nombre definitivo. Linux usa srX para CD/DVD y sdX
    // para todo lo demás.
    usb_storage_assign_name(is_cdrom ? "sr" : "sd", s->name);

    // READ_CAPACITY: tamaño y block size. Para CDs: 2048 B/sector.
    uint32_t last_lba = 0;
    uint32_t block_size = 0;
    rc = scsi_read_capacity_10(s, &last_lba, &block_size);
    if (rc != 0) {
      LOG_ERR("[USB-STG] %s: READ_CAPACITY falló: %d", s->name, rc);
      goto fail_free;
    }
    if (block_size == 0 || last_lba == 0 ||
        (block_size & (block_size - 1)) != 0 || block_size < 512 ||
        block_size > 4096) {
      LOG_ERR("[USB-STG] %s: geometría inválida (last_lba=%u, block_size=%u)",
              s->name, last_lba, block_size);
      goto fail_free;
    }
    s->num_sectors = last_lba + 1;
    s->sector_size = block_size;

    // Registrar block_device_t.
    block_device_t *bdev = (block_device_t *)kzalloc(sizeof(*bdev));
    if (!bdev) {
      rc = -ENOMEM;
      goto fail_free;
    }
    strcpy(bdev->name, s->name);
    bdev->num_sectors = s->num_sectors;
    bdev->sector_size = s->sector_size;
    bdev->is_read_only = is_cdrom ? 1 : 0;
    bdev->ops = &usb_storage_ops;
    bdev->private_data = s;
    s->bdev = bdev;

    rc = blk_register(bdev);
    if (rc < 0) {
      LOG_ERR("[USB-STG] blk_register(%s) falló: %d", s->name, rc);
      kfree(bdev);
      goto fail_free;
    }

    LOG_INFO("[USB-STG] %s: '%s %s', %u sectores de %u B (%u MB)%s", s->name,
             s->inquiry_vendor, s->inquiry_product, s->num_sectors,
             s->sector_size, (s->num_sectors / (1024 * 1024 / s->sector_size)),
             is_cdrom ? " [RO CD]" : "");
    return 1;

  fail_free:
    if (s->data_buf.virt)
      usb_dma_free(&s->data_buf);
    if (s->csw_buf.virt)
      usb_dma_free(&s->csw_buf);
    if (s->cbw_buf.virt)
      usb_dma_free(&s->cbw_buf);
    kfree(s);
    return rc;
  }

  return 0; // no es Mass Storage
}