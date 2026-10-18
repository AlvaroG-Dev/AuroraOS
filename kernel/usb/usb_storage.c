// kernel/usb/usb_storage.c
//
// Mass Storage. Prueba UAS (protocol 0x62) primero; si no, BOT (0x50).

#include "usb_storage.h"
#include "../block.h"
#include "../heap.h"
#include "../klog.h"
#include "../sched.h"
#include "../spinlock.h"
#include "../string.h"
#include "../uaccess.h"
#include "../wait.h"
#include "usb_dma.h"
#include "usb_uas.h"

// ===========================================================================
// BOT
// ===========================================================================
typedef struct __attribute__((packed)) {
  uint32_t signature;
  uint32_t tag;
  uint32_t data_transfer_length;
  uint8_t flags;
  uint8_t lun;
  uint8_t cb_length;
  uint8_t cb[16];
} usb_bot_cbw_t;

typedef struct __attribute__((packed)) {
  uint32_t signature;
  uint32_t tag;
  uint32_t data_residue;
  uint8_t status;
} usb_bot_csw_t;

#define BOT_CBW_SIG 0x43425355u
#define BOT_CSW_SIG 0x53425355u

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2A
#define SCSI_SYNCHRONIZE_CACHE_10 0x35

#define SCSI_TYPE_DISK 0x00
#define SCSI_TYPE_CDROM 0x05
#define SCSI_TYPE_OPTICAL 0x07

#define USB_STORAGE_MAX_XFER (128 * 1024)
#define USB_STORAGE_TIMEOUT_MS 5000

typedef struct {
  usb_device_t *dev;
  usb_endpoint_t *bulk_in;
  usb_endpoint_t *bulk_out;
  uint32_t next_tag;
  uint8_t lun;
  uint8_t dev_type;
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
  spinlock_t queue_lock;
  wait_queue_t queue_wq;
  bio_t *queue_head;
  bio_t *queue_tail;
} usb_storage_t;

static usb_storage_t *g_storage_spawn = NULL;

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

static int usb_storage_bot_xfer(usb_storage_t *s, const uint8_t *cb, int cb_len,
                                void *data, uint32_t data_len, int dir_in,
                                uint32_t timeout_ms) {
  if (!s || !cb || cb_len < 1 || cb_len > 16)
    return -EINVAL;
  if (data_len > s->data_buf.size)
    return -EINVAL;

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
  if (rc != (int)sizeof(*cbw))
    return -EIO;

  if (data && data_len > 0) {
    if (dir_in) {
      rc = s->dev->hcd->bulk_xfer(s->dev, s->bulk_in, s->data_buf.virt,
                                  (int)data_len, 1, timeout_ms);
      if (rc < 0)
        return rc;
      if (rc != (int)data_len)
        return -EIO;
      usb_dma_sync(&s->data_buf, USB_DMA_FROM_DEVICE);
      memcpy(data, s->data_buf.virt, data_len);
    } else {
      memcpy(s->data_buf.virt, data, data_len);
      usb_dma_sync(&s->data_buf, USB_DMA_TO_DEVICE);
      rc = s->dev->hcd->bulk_xfer(s->dev, s->bulk_out, s->data_buf.virt,
                                  (int)data_len, 0, timeout_ms);
      if (rc < 0)
        return rc;
      if (rc != (int)data_len)
        return -EIO;
    }
  }

  memset(s->csw_buf.virt, 0, sizeof(usb_bot_csw_t));
  rc = s->dev->hcd->bulk_xfer(s->dev, s->bulk_in, s->csw_buf.virt,
                              sizeof(usb_bot_csw_t), 1, timeout_ms);
  if (rc < 0)
    return rc;
  if (rc != (int)sizeof(usb_bot_csw_t))
    return -EIO;
  usb_dma_sync(&s->csw_buf, USB_DMA_FROM_DEVICE);

  usb_bot_csw_t *csw = (usb_bot_csw_t *)s->csw_buf.virt;
  if (csw->signature != BOT_CSW_SIG)
    return -EIO;
  if (csw->tag != tag)
    return -EIO;
  if (csw->status != 0)
    return -EIO;
  return 0;
}

static int scsi_test_unit_ready(usb_storage_t *s) {
  uint8_t cb[6] = {SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0};
  return usb_storage_bot_xfer(s, cb, 6, NULL, 0, 0, USB_STORAGE_TIMEOUT_MS);
}
static int scsi_request_sense(usb_storage_t *s, uint8_t *sense, uint32_t len) {
  uint8_t cb[6] = {
      SCSI_REQUEST_SENSE, 0, 0, 0, (uint8_t)(len > 0xFF ? 0xFF : len), 0};
  return usb_storage_bot_xfer(s, cb, 6, sense, len, 1, USB_STORAGE_TIMEOUT_MS);
}
static int scsi_inquiry(usb_storage_t *s) {
  uint8_t buf[36];
  memset(buf, 0, sizeof(buf));
  uint8_t cb[6] = {SCSI_INQUIRY, 0, 0, 0, sizeof(buf), 0};
  int rc = usb_storage_bot_xfer(s, cb, 6, buf, sizeof(buf), 1,
                                USB_STORAGE_TIMEOUT_MS);
  if (rc != 0)
    return rc;
  s->dev_type = buf[0] & 0x1F;
  memcpy(s->inquiry_vendor, buf + 8, 8);
  s->inquiry_vendor[8] = '\0';
  memcpy(s->inquiry_product, buf + 16, 16);
  s->inquiry_product[16] = '\0';
  memcpy(s->inquiry_revision, buf + 32, 4);
  s->inquiry_revision[4] = '\0';
  for (int i = 7; i >= 0 && s->inquiry_vendor[i] == ' '; i--)
    s->inquiry_vendor[i] = '\0';
  for (int i = 15; i >= 0 && s->inquiry_product[i] == ' '; i--)
    s->inquiry_product[i] = '\0';
  for (int i = 3; i >= 0 && s->inquiry_revision[i] == ' '; i--)
    s->inquiry_revision[i] = '\0';
  return 0;
}
static int scsi_read_capacity_10(usb_storage_t *s, uint32_t *last,
                                 uint32_t *bs) {
  uint8_t buf[8];
  memset(buf, 0, sizeof(buf));
  uint8_t cb[10] = {SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  int rc = usb_storage_bot_xfer(s, cb, 10, buf, sizeof(buf), 1,
                                USB_STORAGE_TIMEOUT_MS);
  if (rc != 0)
    return rc;
  *last = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
          ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
  *bs = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
        ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
  return 0;
}
static int scsi_read_10(usb_storage_t *s, uint64_t lba, uint32_t count,
                        void *buf) {
  if (count == 0 || count > 0xFFFF)
    return -EINVAL;
  uint8_t cb[10] = {SCSI_READ_10,
                    0,
                    (lba >> 24) & 0xFF,
                    (lba >> 16) & 0xFF,
                    (lba >> 8) & 0xFF,
                    lba & 0xFF,
                    0,
                    (count >> 8) & 0xFF,
                    count & 0xFF,
                    0};
  return usb_storage_bot_xfer(s, cb, 10, buf, count * s->sector_size, 1,
                              USB_STORAGE_TIMEOUT_MS);
}
static int scsi_write_10(usb_storage_t *s, uint64_t lba, uint32_t count,
                         const void *buf) {
  if (count == 0 || count > 0xFFFF)
    return -EINVAL;
  uint8_t cb[10] = {SCSI_WRITE_10,
                    0,
                    (lba >> 24) & 0xFF,
                    (lba >> 16) & 0xFF,
                    (lba >> 8) & 0xFF,
                    lba & 0xFF,
                    0,
                    (count >> 8) & 0xFF,
                    count & 0xFF,
                    0};
  return usb_storage_bot_xfer(s, cb, 10, (void *)buf, count * s->sector_size, 0,
                              USB_STORAGE_TIMEOUT_MS);
}
static int scsi_sync_cache(usb_storage_t *s) {
  uint8_t cb[10] = {SCSI_SYNCHRONIZE_CACHE_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  return usb_storage_bot_xfer(s, cb, 10, NULL, 0, 0, USB_STORAGE_TIMEOUT_MS);
}

static int usb_storage_do_read(usb_storage_t *s, uint64_t lba, uint32_t count,
                               void *buf) {
  uint8_t *p = (uint8_t *)buf;
  uint32_t max_blocks = (uint32_t)(s->data_buf.size / s->sector_size);
  if (max_blocks == 0 || max_blocks > 0xFFFF)
    max_blocks = 0xFFFF;
  while (count > 0) {
    uint32_t n = count < max_blocks ? count : max_blocks;
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
    uint32_t n = count < max_blocks ? count : max_blocks;
    int rc = scsi_write_10(s, lba, n, p);
    if (rc != 0)
      return rc;
    p += (uint64_t)n * s->sector_size;
    lba += n;
    count -= n;
  }
  (void)scsi_sync_cache(s);
  return 0;
}

static bool storage_queue_not_empty(void *arg) {
  return ((usb_storage_t *)arg)->queue_head != NULL;
}

static void usb_storage_worker(void) {
  usb_storage_t *s = g_storage_spawn;
  g_storage_spawn = NULL;
  if (!s)
    return;
  LOG_INFO("[USB-STG] %s: worker iniciado", s->name);
  while (1) {
    wait_event(&s->queue_wq, storage_queue_not_empty, s);
    unsigned long flags = spin_lock_irqsave(&s->queue_lock);
    bio_t *bio = s->queue_head;
    if (bio) {
      s->queue_head = bio->next;
      if (!s->queue_head)
        s->queue_tail = NULL;
      bio->next = NULL;
    }
    spin_unlock_irqrestore(&s->queue_lock, flags);
    if (!bio)
      continue;
    int rc;
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
  }
}

static int usb_storage_spawn_worker(usb_storage_t *s) {
  while (g_storage_spawn != NULL)
    sched_yield();
  g_storage_spawn = s;
  if (!sched_create_task(usb_storage_worker)) {
    g_storage_spawn = NULL;
    return -ENOMEM;
  }
  while (g_storage_spawn != NULL)
    sched_yield();
  return 0;
}

static int usb_storage_submit(block_device_t *bdev, bio_t *bio) {
  usb_storage_t *s = (usb_storage_t *)bdev->private_data;
  if (!s)
    return -ENODEV;
  bio->error = 0;
  bio->next = NULL;
  unsigned long flags = spin_lock_irqsave(&s->queue_lock);
  if (!s->queue_head) {
    s->queue_head = bio;
    s->queue_tail = bio;
  } else {
    s->queue_tail->next = bio;
    s->queue_tail = bio;
  }
  spin_unlock_irqrestore(&s->queue_lock, flags);
  wake_up_one(&s->queue_wq);
  return -EINPROGRESS;
}
static void usb_storage_flush_marker(block_device_t *bdev) { (void)bdev; }
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

  // ------------------------------------------------------------------
  // 1. Intenta UAS (class 0x08 / subclass 0x06 / protocol 0x62).
  // ------------------------------------------------------------------
  for (int i = 0; i < cfg->num_interface_structs; i++) {
    usb_interface_t *itf = &cfg->interfaces[i];
    if (itf->class != 0x08 || itf->subclass != 0x06)
      continue;
    if (itf->protocol != 0x62)
      continue;

    LOG_INFO("[USB-STG] iface %u UAS detectada (%d endpoints)", itf->number,
             itf->num_endpoints);

    usb_endpoint_t *ep_by_addr[0x10];
    for (int k = 0; k < 0x10; k++)
      ep_by_addr[k] = NULL;

    for (int j = 0; j < itf->num_endpoints; j++) {
      usb_endpoint_t *ep = &itf->endpoints[j];
      LOG_DEBUG("[USB-STG]   ep[%d] addr=0x%02x type=%d mps=%u interval=%u", j,
                ep->addr, ep->type, ep->max_packet, ep->interval);
      ep_by_addr[ep->addr & 0x0F] = ep; // indexamos por dirección de endpoint
    }

    // QEMU's usb-uas (y la spec UAS 1.0 en su convención más común):
    //   EP1 OUT  = Command IU
    //   EP2 IN   = Status IU
    //   EP3 IN   = Data-In
    //   EP4 OUT  = Data-Out
    //
    // Buscamos por dirección exacta (0x01, 0x82, 0x83, 0x04) y no por
    // "primer IN / primer OUT", que era la causa del bug: asignábamos
    // 0x82 como Data-In (siendo Status) y 0x83 como Status (siendo Data-In).
    usb_endpoint_t *cmd_ep = ep_by_addr[0x01];
    usb_endpoint_t *status_ep = ep_by_addr[0x02];  // 0x82 & 0x0F
    usb_endpoint_t *data_in_ep = ep_by_addr[0x03]; // 0x83 & 0x0F
    usb_endpoint_t *data_out_ep = ep_by_addr[0x04];

    if (!cmd_ep || !status_ep || !data_in_ep || !data_out_ep) {
      LOG_WARN("[USB-STG] UAS: endpoints incompletos "
               "(cmd=%p status=%p data_in=%p data_out=%p)",
               (void *)cmd_ep, (void *)status_ep, (void *)data_in_ep,
               (void *)data_out_ep);
      continue;
    }

    int rc =
        usb_uas_probe(dev, itf, cmd_ep, status_ep, data_in_ep, data_out_ep);
    if (rc > 0)
      return rc;
    LOG_WARN("[USB-STG] UAS falló (%d), probando BOT si está disponible", rc);
  }

  // ------------------------------------------------------------------
  // 2. BOT clásico (class 0x08 / subclass 0x06 / protocol 0x50).
  // ------------------------------------------------------------------
  for (int i = 0; i < cfg->num_interface_structs; i++) {
    usb_interface_t *itf = &cfg->interfaces[i];
    if (itf->class != 0x08 || itf->subclass != 0x06 || itf->protocol != 0x50)
      continue;

    usb_endpoint_t *bulk_in = NULL, *bulk_out = NULL;
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
      LOG_ERR("[USB-STG] BOT iface %u: falta bulk IN/OUT", itf->number);
      return -ENODEV;
    }

    LOG_INFO("[USB-STG] iface %u BOT (EP%u IN, EP%u OUT)", itf->number,
             bulk_in->addr & 0x0F, bulk_out->addr & 0x0F);

    usb_storage_t *s = (usb_storage_t *)kzalloc(sizeof(*s));
    if (!s)
      return -ENOMEM;
    s->dev = dev;
    s->bulk_in = bulk_in;
    s->bulk_out = bulk_out;
    s->next_tag = 0;
    s->lun = 0;
    spin_init(&s->queue_lock);
    wait_queue_init(&s->queue_wq);
    strcpy(s->name, "usbstg");

    int rc = usb_dma_alloc(sizeof(usb_bot_cbw_t), &s->cbw_buf);
    if (rc != 0)
      goto fail_free;
    rc = usb_dma_alloc(sizeof(usb_bot_csw_t), &s->csw_buf);
    if (rc != 0)
      goto fail_free;
    rc = usb_dma_alloc(USB_STORAGE_MAX_XFER, &s->data_buf);
    if (rc != 0)
      goto fail_free;

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

    rc = scsi_test_unit_ready(s);
    for (int r = 0; r < 3 && rc != 0; r++) {
      uint8_t sense[18];
      (void)scsi_request_sense(s, sense, sizeof(sense));
      rc = scsi_test_unit_ready(s);
    }
    if (rc != 0) {
      LOG_ERR("[USB-STG] TEST_UNIT_READY falló: %d", rc);
      goto fail_free;
    }

    rc = scsi_inquiry(s);
    if (rc != 0) {
      LOG_ERR("[USB-STG] INQUIRY falló: %d", rc);
      goto fail_free;
    }

    int is_cdrom =
        (s->dev_type == SCSI_TYPE_CDROM || s->dev_type == SCSI_TYPE_OPTICAL);
    usb_storage_assign_name(is_cdrom ? "sr" : "sd", s->name);

    uint32_t last = 0, bs = 0;
    rc = scsi_read_capacity_10(s, &last, &bs);
    if (rc != 0) {
      LOG_ERR("[USB-STG] %s: READ_CAPACITY falló: %d", s->name, rc);
      goto fail_free;
    }
    if (bs == 0 || last == 0 || (bs & (bs - 1)) != 0 || bs < 512 || bs > 4096) {
      LOG_ERR("[USB-STG] %s: geometría inválida (last=%u bs=%u)", s->name, last,
              bs);
      goto fail_free;
    }
    s->num_sectors = last + 1;
    s->sector_size = bs;

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

    rc = usb_storage_spawn_worker(s);
    if (rc != 0) {
      LOG_ERR("[USB-STG] %s: worker spawn falló: %d", s->name, rc);
      blk_unregister(bdev);
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

  return 0;
}