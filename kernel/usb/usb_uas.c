// kernel/usb/usb_uas.c
//
// Driver UAS (USB Attached SCSI). Protocolo UAS 1.0.
//
// Layout de IUs (spec UAS 1.0 §7.1, §7.2 — consistente con Linux
// drivers/usb/storage/uas.c y QEMU hw/usb/dev-uas.c):
//
//   Command IU (32 bytes):
//     [0]     iu_id = 0x01 (COMMAND)
//     [1]     reserved
//     [2..3]  tag (BE)
//     [4]     prio_task_attr (0 = simple)
//     [5]     reserved
//     [6]     add_cdb_length
//     [7]     reserved
//     [8..15] lun[8]
//     [16..31] cdb[16]
//
//   Response IU (12 bytes): caso normal GOOD.
//     [0]     iu_id = 0x04 (RESPONSE)
//     [1]     reserved
//     [2..3]  tag (BE)
//     [4]     status (0 = OK)
//     [5..7]  reserved
//     [8..11] residual (BE)
//
//   Sense IU (112 bytes = 16 header + 96 sense): CHECK CONDITION inline.
//     [0]     iu_id = 0x03 (SENSE)
//     [1]     reserved
//     [2..3]  tag (BE)
//     [4..5]  status_qual (BE)
//     [6]     status
//     [7..13] reserved
//     [14..15] sense_length (BE)
//     [16..111] sense[96]
//
//   IMPORTANTE sobre QEMU's usb-uas:
//     - Entrega CHECK CONDITION como SENSE IU (0x03) con la sense data
//       INLINE dentro del mismo IU. No usa Response IU para errores.
//     - Después de eso, mantiene el modo Sense IU para el resto de la
//       sesión, incluso con status=0 (GOOD). Es raro pero es lo que hace.
//     - NO acepta REQUEST_SENSE después de un CHECK CONDITION inline:
//       usb_uas_handle_data devuelve "no inflight request" (STALL).
//     - Por eso el probe reintenta TEST_UNIT_READY (que limpia la UA
//       con la siguiente transacción) en vez de mandar REQUEST_SENSE.
//
// Endpoints (convención UAS / QEMU's usb-uas):
//   EP1 OUT (0x01) = Command IU
//   EP2 IN  (0x82) = Status IU (Response o Sense)
//   EP3 IN  (0x83) = Data-In
//   EP4 OUT (0x04) = Data-Out
//
// Status URB post-submit (no pre-submit): QEMU guarda el USBPacket IN
// huérfano en uas->status_packet y su xHCI lo cancela, dejando el
// puntero colgando. Pedir el Status después de Command+Data funciona.

#include "usb_uas.h"
#include "../block.h"
#include "../heap.h"
#include "../klog.h"
#include "../sched.h"
#include "../spinlock.h"
#include "../string.h"
#include "../uaccess.h"
#include "../wait.h"
#include "usb_core.h"
#include "usb_dma.h"

// ===========================================================================
// UAS IU IDs
// ===========================================================================
#define UAS_IU_COMMAND 0x01
#define UAS_IU_SENSE 0x03
#define UAS_IU_RESPONSE 0x04
#define UAS_IU_TASK_MGMT 0x05
#define UAS_IU_READ_READY 0x06
#define UAS_IU_WRITE_READY 0x07

#define UAS_DIR_NONE 0
#define UAS_DIR_IN 1
#define UAS_DIR_OUT 2

// ===========================================================================
// Structs de IU
// ===========================================================================
typedef struct __attribute__((packed)) {
  uint8_t iu_id;
  uint8_t reserved;
  uint16_t tag;
  uint8_t prio_task_attr;
  uint8_t reserved5;
  uint8_t add_cdb_length;
  uint8_t reserved7;
  uint8_t lun[8];
  uint8_t cdb[16];
} uas_cmd_iu_t;

_Static_assert(sizeof(uas_cmd_iu_t) == 32, "uas_cmd_iu_t != 32B");

typedef struct __attribute__((packed)) {
  uint8_t iu_id;
  uint8_t reserved;
  uint16_t tag;
  uint8_t status;
  uint8_t reserved2[3];
  uint32_t residual;
} uas_response_iu_t;

_Static_assert(sizeof(uas_response_iu_t) == 12, "uas_response_iu_t != 12B");

typedef struct __attribute__((packed)) {
  uint8_t iu_id;
  uint8_t reserved;
  uint16_t tag;
  uint16_t status_qual;
  uint8_t status;
  uint8_t reserved2[7];
  uint16_t sense_length;
  uint8_t sense[96];
} uas_sense_iu_t;

_Static_assert(sizeof(uas_sense_iu_t) == 112, "uas_sense_iu_t != 112B");

// Buffer Status IN: cubre Sense IU (112 B) completo. QEMU's usb-uas
// hace usb_packet_copy con sizeof(UASSenseIU)=112 cuando el comando
// devuelve CHECK CONDITION; con un iov más pequeño dispara:
//   hw/usb/core.c:612: usb_packet_copy: Assertion
//   `p->actual_length + bytes <= iov->size' failed.
#define UAS_STATUS_BUF_SIZE 112

// ===========================================================================
// SCSI opcodes y tipos
// ===========================================================================
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

#define UAS_MAX_XFER (128 * 1024)
#define UAS_TIMEOUT 5000

typedef struct {
  usb_device_t *dev;
  usb_endpoint_t *cmd_ep;
  usb_endpoint_t *status_ep;
  usb_endpoint_t *data_in_ep;
  usb_endpoint_t *data_out_ep;

  uint8_t lun;
  uint16_t next_tag;
  uint8_t dev_type;

  uint32_t num_sectors;
  uint32_t sector_size;
  char name[8];
  block_device_t *bdev;

  usb_dma_buf_t cmd_buf;
  usb_dma_buf_t status_buf;
  usb_dma_buf_t data_buf;

  spinlock_t lock;
  wait_queue_t queue_wq;
  bio_t *queue_head;
  bio_t *queue_tail;

  char inquiry_vendor[9];
  char inquiry_product[17];
  char inquiry_revision[5];
} uas_dev_t;

static uas_dev_t *g_uas_spawn = NULL;

// ===========================================================================
// clflush + mfence para que QEMU vea los bytes frescos en el buffer
// físico (QEMU's usb-uas lee con address_space_read sin pasar por la
// caché del guest).
// ===========================================================================
static void uas_flush_range(const void *virt, size_t len) {
  uintptr_t start = (uintptr_t)virt & ~(uintptr_t)63;
  uintptr_t end = ((uintptr_t)virt + len + 63) & ~(uintptr_t)63;
  for (uintptr_t a = start; a < end; a += 64)
    __asm__ volatile("clflush (%0)" : : "r"((const void *)a) : "memory");
  __asm__ volatile("mfence" ::: "memory");
}

// ===========================================================================
// Helpers de espera asíncrona.
// ===========================================================================
typedef struct {
  wait_queue_t wq;
  volatile int done;
  int error;
} uas_urb_wait_t;

static bool uas_urb_is_done(void *arg) {
  return ((uas_urb_wait_t *)arg)->done != 0;
}

static void uas_urb_complete(usb_bulk_urb_t *urb) {
  uas_urb_wait_t *w = (uas_urb_wait_t *)urb->context;
  if (!w)
    return;
  w->error = urb->status;
  w->done = 1;
  wake_up_all(&w->wq);
}

// ===========================================================================
// Ejecución de una operación UAS:
//   Command OUT → Data (si aplica) → Status IN (post-submit).
// ===========================================================================
static int uas_execute(uas_dev_t *u, const uint8_t *cdb, int cdb_len,
                       void *data, uint32_t data_len, int dir) {
  if (!u || !cdb || cdb_len < 6 || cdb_len > 16)
    return -EINVAL;
  if (data_len > u->data_buf.size)
    return -EINVAL;
  if (!u->dev || !u->dev->hcd || !u->dev->hcd->bulk_xfer_phys ||
      !u->dev->hcd->bulk_submit)
    return -ENODEV;

  uint16_t tag = ++u->next_tag;
  if (tag == 0 || tag >= 16)
    tag = u->next_tag = 1;

  // -------------------------------------------------------------------
  // 1. Command IU → cmd_ep (EP1 OUT).
  // -------------------------------------------------------------------
  uas_cmd_iu_t *cmd = (uas_cmd_iu_t *)u->cmd_buf.virt;
  memset(cmd, 0, sizeof(*cmd));
  cmd->iu_id = UAS_IU_COMMAND;
  cmd->tag = __builtin_bswap16(tag);
  cmd->lun[0] = u->lun;
  memcpy(cmd->cdb, cdb, cdb_len);
  uas_flush_range(u->cmd_buf.virt, sizeof(*cmd));

  LOG_DEBUG("[UAS] %s: cmd_iu tag=%u lun=%u cdb[0]=0x%02x", u->name, tag,
            u->lun, cdb[0]);

  uas_urb_wait_t cmd_wait;
  wait_queue_init(&cmd_wait.wq);
  cmd_wait.done = 0;
  cmd_wait.error = 0;

  usb_bulk_urb_t cmd_urb;
  memset(&cmd_urb, 0, sizeof(cmd_urb));
  cmd_urb.dev = u->dev;
  cmd_urb.ep = u->cmd_ep;
  cmd_urb.buf = u->cmd_buf.virt;
  cmd_urb.len = sizeof(*cmd);
  cmd_urb.is_in = 0;
  cmd_urb.complete = uas_urb_complete;
  cmd_urb.context = &cmd_wait;
  cmd_urb.dma_buf = &u->cmd_buf;

  int rc = u->dev->hcd->bulk_submit(u->dev, u->cmd_ep, &cmd_urb);
  if (rc == 0)
    wait_event(&cmd_wait.wq, uas_urb_is_done, &cmd_wait);
  if (rc == 0)
    rc = cmd_wait.error ? cmd_wait.error : cmd_urb.actual_len;
  if (rc < 0) {
    LOG_ERR("[UAS] %s: Command IU falló: %d", u->name, rc);
    return rc;
  }
  if (rc != (int)sizeof(*cmd)) {
    LOG_ERR("[UAS] %s: Command IU short (rc=%d, want=%zu)", u->name, rc,
            sizeof(*cmd));
    return -EIO;
  }

  // -------------------------------------------------------------------
  // 2. Data stage (si aplica). Síncrono.
  // -------------------------------------------------------------------
  if (data && data_len > 0) {
    if (dir == UAS_DIR_IN) {
      uas_flush_range(u->data_buf.virt, data_len);

      rc = u->dev->hcd->bulk_xfer_phys(u->dev, u->data_in_ep, u->data_buf.phys,
                                       u->data_buf.virt, (int)data_len, 1,
                                       UAS_TIMEOUT);
      if (rc < 0) {
        LOG_ERR("[UAS] %s: Data-In falló: %d", u->name, rc);
        return rc;
      }
      if (rc != (int)data_len) {
        LOG_ERR("[UAS] %s: Data-In short (rc=%d, want=%u)", u->name, rc,
                data_len);
        return -EIO;
      }

      uas_flush_range(u->data_buf.virt, data_len);
      memcpy(data, u->data_buf.virt, data_len);
    } else if (dir == UAS_DIR_OUT) {
      memcpy(u->data_buf.virt, data, data_len);
      uas_flush_range(u->data_buf.virt, data_len);

      rc = u->dev->hcd->bulk_xfer_phys(u->dev, u->data_out_ep, u->data_buf.phys,
                                       u->data_buf.virt, (int)data_len, 0,
                                       UAS_TIMEOUT);
      if (rc < 0) {
        LOG_ERR("[UAS] %s: Data-Out falló: %d", u->name, rc);
        return rc;
      }
      if (rc != (int)data_len) {
        LOG_ERR("[UAS] %s: Data-Out short (rc=%d, want=%u)", u->name, rc,
                data_len);
        return -EIO;
      }
    }
  }

  // -------------------------------------------------------------------
  // 3. Status IN (post-submit). Buffer 112 B para caber Sense IU.
  // -------------------------------------------------------------------
  memset(u->status_buf.virt, 0, UAS_STATUS_BUF_SIZE);
  uas_flush_range(u->status_buf.virt, UAS_STATUS_BUF_SIZE);

  uas_urb_wait_t status_wait;
  wait_queue_init(&status_wait.wq);
  status_wait.done = 0;
  status_wait.error = 0;

  usb_bulk_urb_t status_urb;
  memset(&status_urb, 0, sizeof(status_urb));
  status_urb.dev = u->dev;
  status_urb.ep = u->status_ep;
  status_urb.buf = u->status_buf.virt;
  status_urb.len = UAS_STATUS_BUF_SIZE;
  status_urb.is_in = 1;
  status_urb.complete = uas_urb_complete;
  status_urb.context = &status_wait;
  status_urb.dma_buf = &u->status_buf;

  rc = u->dev->hcd->bulk_submit(u->dev, u->status_ep, &status_urb);
  if (rc < 0) {
    LOG_ERR("[UAS] %s: Status IU submit falló: %d", u->name, rc);
    return rc;
  }

  // Timeout duro por TSC (5 s).
  extern uint64_t klog_get_tsc_freq(void);
  extern void sched_yield(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    freq = 2000000000ULL;
  uint64_t max_cycles = (freq / 1000) * UAS_TIMEOUT;

  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t0 = ((uint64_t)hi << 32) | lo;

  int iter = 0;
  while (!status_wait.done) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if ((((uint64_t)hi << 32) | lo) - t0 >= max_cycles) {
      LOG_ERR("[UAS] %s: Status IU TIMEOUT (tag=%u)", u->name, tag);
      return -ETIMEDOUT;
    }
    if ((++iter & 0x3F) == 0)
      sched_yield();
    else
      __asm__ volatile("pause");
  }

  if (status_wait.error != 0) {
    LOG_ERR("[UAS] %s: Status URB error: %d", u->name, status_wait.error);
    return status_wait.error;
  }

  uas_flush_range(u->status_buf.virt, UAS_STATUS_BUF_SIZE);

  int actual = status_urb.actual_len;
  if (actual < 1) {
    LOG_ERR("[UAS] %s: Status IU vacío (actual=%d)", u->name, actual);
    return -EIO;
  }

  uint8_t iu_id = ((uint8_t *)u->status_buf.virt)[0];

  if (iu_id == UAS_IU_RESPONSE) {
    if (actual < (int)sizeof(uas_response_iu_t)) {
      LOG_ERR("[UAS] %s: RESPONSE IU short (actual=%d)", u->name, actual);
      return -EIO;
    }
    uas_response_iu_t *r = (uas_response_iu_t *)u->status_buf.virt;
    LOG_DEBUG("[UAS] %s: RESPONSE status=%u tag=%u residual=%u", u->name,
              r->status, __builtin_bswap16(r->tag),
              __builtin_bswap32(r->residual));
    if (__builtin_bswap16(r->tag) != tag) {
      LOG_ERR("[UAS] %s: response tag mismatch (%u != %u)", u->name,
              __builtin_bswap16(r->tag), tag);
      return -EIO;
    }
    if (r->status != 0) {
      LOG_ERR("[UAS] %s: SCSI status=%u residual=%u", u->name, r->status,
              __builtin_bswap32(r->residual));
      return -EIO;
    }
    return 0;
  }

  if (iu_id == UAS_IU_SENSE) {
    // Sense IU: 112 B con sense data inline. QEMU lo usa para
    // CHECK CONDITION (status != 0) y también para éxito (status=0)
    // cuando entra en modo "Sense IU" tras el primer CHECK CONDITION.
    if (actual < 16) {
      LOG_ERR("[UAS] %s: SENSE IU short (actual=%d)", u->name, actual);
      return -EIO;
    }
    uas_sense_iu_t *s = (uas_sense_iu_t *)u->status_buf.virt;
    uint16_t s_tag = __builtin_bswap16(s->tag);
    uint16_t s_len = __builtin_bswap16(s->sense_length);
    LOG_DEBUG("[UAS] %s: SENSE status=%u tag=%u sense_len=%u", u->name,
              s->status, s_tag, s_len);
    if (s_tag != tag) {
      LOG_ERR("[UAS] %s: sense tag mismatch (%u != %u)", u->name, s_tag, tag);
      return -EIO;
    }
    if (s->status == 0) {
      // QEMU usa Sense IU con status=0 para indicar GOOD cuando ya
      // ha entrado en modo Sense IU. NO es un error.
      return 0;
    }
    // CHECK CONDITION: loguear la sense data para diagnóstico. El
    // caller (probe) reintenta el comando; NO mandar REQUEST_SENSE
    // (la sense ya está inline).
    if (s_len >= 14 && s_len <= 96) {
      LOG_DEBUG("[UAS] sense: key=0x%02x asc=0x%02x ascq=0x%02x",
                s->sense[2] & 0x0F, s->sense[12], s->sense[13]);
    }
    return -EIO;
  }

  LOG_ERR("[UAS] %s: unknown status IU id 0x%02x (actual=%d)", u->name, iu_id,
          actual);
  return -EIO;
}

// ===========================================================================
// Comandos SCSI
// ===========================================================================
static int uas_test_unit_ready(uas_dev_t *u) {
  uint8_t cdb[6] = {SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0};
  return uas_execute(u, cdb, 6, NULL, 0, UAS_DIR_NONE);
}

// uas_request_sense se deja para hardware real que no entregue sense
// inline. Con QEMU's usb-uas NO se debe llamar: rechaza el Data-In
// con "no inflight request" y deja el transfer en timeout. El probe
// no la usa. Marcada unused para evitar warnings.
__attribute__((unused)) static int
uas_request_sense(uas_dev_t *u, uint8_t *sense, uint32_t sense_len) {
  uint8_t cdb[6] = {SCSI_REQUEST_SENSE,
                    0,
                    0,
                    0,
                    (uint8_t)(sense_len > 0xFF ? 0xFF : sense_len),
                    0};
  return uas_execute(u, cdb, 6, sense, sense_len, UAS_DIR_IN);
}

static int uas_inquiry(uas_dev_t *u) {
  uint8_t buf[36];
  memset(buf, 0, sizeof(buf));
  uint8_t cdb[6] = {SCSI_INQUIRY, 0, 0, 0, sizeof(buf), 0};
  int rc = uas_execute(u, cdb, 6, buf, sizeof(buf), UAS_DIR_IN);
  if (rc != 0)
    return rc;
  u->dev_type = buf[0] & 0x1F;
  memcpy(u->inquiry_vendor, buf + 8, 8);
  u->inquiry_vendor[8] = '\0';
  memcpy(u->inquiry_product, buf + 16, 16);
  u->inquiry_product[16] = '\0';
  memcpy(u->inquiry_revision, buf + 32, 4);
  u->inquiry_revision[4] = '\0';
  for (int i = 7; i >= 0 && u->inquiry_vendor[i] == ' '; i--)
    u->inquiry_vendor[i] = '\0';
  for (int i = 15; i >= 0 && u->inquiry_product[i] == ' '; i--)
    u->inquiry_product[i] = '\0';
  for (int i = 3; i >= 0 && u->inquiry_revision[i] == ' '; i--)
    u->inquiry_revision[i] = '\0';
  return 0;
}

static int uas_read_capacity_10(uas_dev_t *u, uint32_t *last_lba,
                                uint32_t *bs) {
  uint8_t buf[8];
  memset(buf, 0, sizeof(buf));
  uint8_t cdb[10] = {SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  int rc = uas_execute(u, cdb, 10, buf, sizeof(buf), UAS_DIR_IN);
  if (rc != 0)
    return rc;
  *last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
              ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
  *bs = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
        ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
  return 0;
}

static int uas_read_10(uas_dev_t *u, uint64_t lba, uint32_t count, void *buf) {
  uint8_t cdb[10] = {SCSI_READ_10,
                     0,
                     (lba >> 24) & 0xFF,
                     (lba >> 16) & 0xFF,
                     (lba >> 8) & 0xFF,
                     lba & 0xFF,
                     0,
                     (count >> 8) & 0xFF,
                     count & 0xFF,
                     0};
  uint32_t bytes = count * u->sector_size;
  return uas_execute(u, cdb, 10, buf, bytes, UAS_DIR_IN);
}

static int uas_write_10(uas_dev_t *u, uint64_t lba, uint32_t count,
                        const void *buf) {
  uint8_t cdb[10] = {SCSI_WRITE_10,
                     0,
                     (lba >> 24) & 0xFF,
                     (lba >> 16) & 0xFF,
                     (lba >> 8) & 0xFF,
                     lba & 0xFF,
                     0,
                     (count >> 8) & 0xFF,
                     count & 0xFF,
                     0};
  uint32_t bytes = count * u->sector_size;
  return uas_execute(u, cdb, 10, (void *)buf, bytes, UAS_DIR_OUT);
}

static int uas_sync_cache(uas_dev_t *u) {
  uint8_t cdb[10] = {SCSI_SYNCHRONIZE_CACHE_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  return uas_execute(u, cdb, 10, NULL, 0, UAS_DIR_NONE);
}

// ===========================================================================
// Worker + submit
// ===========================================================================
static bool uas_queue_not_empty(void *arg) {
  return ((uas_dev_t *)arg)->queue_head != NULL;
}

static void uas_do_read(uas_dev_t *u, bio_t *bio) {
  uint8_t *p = (uint8_t *)bio->buf;
  uint32_t left = bio->count;
  uint64_t lba = bio->lba;
  uint32_t max_blocks = (uint32_t)(u->data_buf.size / u->sector_size);
  if (max_blocks == 0 || max_blocks > 0xFFFF)
    max_blocks = 0xFFFF;
  while (left > 0) {
    uint32_t n = left < max_blocks ? left : max_blocks;
    int rc = uas_read_10(u, lba, n, p);
    if (rc != 0) {
      bio_endio(bio, rc);
      return;
    }
    p += (uint64_t)n * u->sector_size;
    lba += n;
    left -= n;
  }
  bio_endio(bio, 0);
}

static void uas_do_write(uas_dev_t *u, bio_t *bio) {
  const uint8_t *p = (const uint8_t *)bio->buf;
  uint32_t left = bio->count;
  uint64_t lba = bio->lba;
  uint32_t max_blocks = (uint32_t)(u->data_buf.size / u->sector_size);
  if (max_blocks == 0 || max_blocks > 0xFFFF)
    max_blocks = 0xFFFF;
  while (left > 0) {
    uint32_t n = left < max_blocks ? left : max_blocks;
    int rc = uas_write_10(u, lba, n, p);
    if (rc != 0) {
      bio_endio(bio, rc);
      return;
    }
    p += (uint64_t)n * u->sector_size;
    lba += n;
    left -= n;
  }
  bio_endio(bio, uas_sync_cache(u));
}

static void uas_worker(void) {
  uas_dev_t *u = g_uas_spawn;
  g_uas_spawn = NULL;
  if (!u) {
    LOG_ERR("[UAS] worker sin device");
    return;
  }
  LOG_INFO("[UAS] %s: worker iniciado", u->name);
  while (1) {
    wait_event(&u->queue_wq, uas_queue_not_empty, u);
    unsigned long flags = spin_lock_irqsave(&u->lock);
    bio_t *bio = u->queue_head;
    if (bio) {
      u->queue_head = bio->next;
      if (!u->queue_head)
        u->queue_tail = NULL;
      bio->next = NULL;
    }
    spin_unlock_irqrestore(&u->lock, flags);
    if (!bio)
      continue;
    switch (bio->op) {
    case BIO_READ:
      uas_do_read(u, bio);
      break;
    case BIO_WRITE:
      uas_do_write(u, bio);
      break;
    case BIO_FLUSH:
      bio_endio(bio, uas_sync_cache(u));
      break;
    default:
      bio_endio(bio, -EINVAL);
      break;
    }
  }
}

static int uas_submit(block_device_t *bdev, bio_t *bio) {
  uas_dev_t *u = (uas_dev_t *)bdev->private_data;
  if (!u)
    return -ENODEV;
  bio->error = 0;
  bio->next = NULL;
  unsigned long flags = spin_lock_irqsave(&u->lock);
  if (!u->queue_head) {
    u->queue_head = bio;
    u->queue_tail = bio;
  } else {
    u->queue_tail->next = bio;
    u->queue_tail = bio;
  }
  spin_unlock_irqrestore(&u->lock, flags);
  wake_up_one(&u->queue_wq);
  return -EINPROGRESS;
}

static void uas_dump(block_device_t *bdev) {
  uas_dev_t *u = (uas_dev_t *)bdev->private_data;
  if (!u)
    return;
  LOG_INFO("    UAS %s: '%s %s' %u B/sector, %u sectores%s", u->name,
           u->inquiry_vendor, u->inquiry_product, u->sector_size,
           u->num_sectors, bdev->is_read_only ? " [RO]" : "");
}

static block_ops_t uas_ops = {
    .submit = uas_submit, .flush = NULL, .dump = uas_dump};

static int uas_spawn_worker(uas_dev_t *u) {
  while (g_uas_spawn != NULL)
    sched_yield();
  g_uas_spawn = u;
  if (!sched_create_task(uas_worker)) {
    g_uas_spawn = NULL;
    return -ENOMEM;
  }
  while (g_uas_spawn != NULL)
    sched_yield();
  return 0;
}

static void uas_assign_name(const char *prefix, char *out) {
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
// Probe
// ===========================================================================
int usb_uas_probe(usb_device_t *dev, usb_interface_t *itf,
                  usb_endpoint_t *cmd_ep, usb_endpoint_t *status_ep,
                  usb_endpoint_t *data_in_ep, usb_endpoint_t *data_out_ep) {
  if (!dev || !itf || !cmd_ep || !status_ep || !data_in_ep || !data_out_ep)
    return -EINVAL;

  uas_dev_t *u = kzalloc(sizeof(*u));
  if (!u)
    return -ENOMEM;
  u->dev = dev;
  u->cmd_ep = cmd_ep;
  u->status_ep = status_ep;
  u->data_in_ep = data_in_ep;
  u->data_out_ep = data_out_ep;
  // Sin streams. QEMU's usb-uas no usa set_stream_id; si el driver
  // pone stream_id != 0, el xHCI camina un stream context que QEMU
  // nunca toca y el Status URB nunca completa.
  cmd_ep->stream_id = 0;
  status_ep->stream_id = 0;
  data_in_ep->stream_id = 0;
  data_out_ep->stream_id = 0;
  u->lun = 0;
  u->next_tag = 0;
  spin_init(&u->lock);
  wait_queue_init(&u->queue_wq);
  strcpy(u->name, "uas");

  int rc = usb_dma_alloc(sizeof(uas_cmd_iu_t), &u->cmd_buf);
  if (rc != 0)
    goto fail;
  rc = usb_dma_alloc(UAS_STATUS_BUF_SIZE, &u->status_buf);
  if (rc != 0)
    goto fail;
  rc = usb_dma_alloc(UAS_MAX_XFER, &u->data_buf);
  if (rc != 0)
    goto fail;

  rc = dev->hcd->configure_ep(dev, cmd_ep);
  if (rc != 0) {
    LOG_ERR("[UAS] configure_ep(cmd) falló: %d", rc);
    goto fail;
  }
  rc = dev->hcd->configure_ep(dev, status_ep);
  if (rc != 0) {
    LOG_ERR("[UAS] configure_ep(status) falló: %d", rc);
    goto fail;
  }
  rc = dev->hcd->configure_ep(dev, data_in_ep);
  if (rc != 0) {
    LOG_ERR("[UAS] configure_ep(data_in) falló: %d", rc);
    goto fail;
  }
  rc = dev->hcd->configure_ep(dev, data_out_ep);
  if (rc != 0) {
    LOG_ERR("[UAS] configure_ep(data_out) falló: %d", rc);
    goto fail;
  }

  // TEST_UNIT_READY con reintentos. QEMU's usb-uas entrega el sense
  // de UNIT ATTENTION inline (Sense IU), así que NO hay que mandar
  // REQUEST_SENSE (QEMU rechaza el Data-In con "no inflight request").
  // Simplemente reintentamos TEST_UNIT_READY hasta que el UA se
  // limpie (cada comando que pasa tras un UA consume el UA).
  rc = uas_test_unit_ready(u);
  for (int r = 0; r < 5 && rc != 0; r++) {
    LOG_DEBUG("[UAS] %s: TEST_UNIT_READY retry %d", u->name, r + 1);
    rc = uas_test_unit_ready(u);
  }
  if (rc != 0) {
    LOG_ERR("[UAS] TEST_UNIT_READY falló: %d", rc);
    goto fail;
  }

  rc = uas_inquiry(u);
  if (rc != 0) {
    LOG_ERR("[UAS] INQUIRY falló: %d", rc);
    goto fail;
  }

  int is_cdrom =
      (u->dev_type == SCSI_TYPE_CDROM || u->dev_type == SCSI_TYPE_OPTICAL);
  uas_assign_name(is_cdrom ? "sr" : "sd", u->name);

  uint32_t last_lba = 0, bs = 0;
  rc = uas_read_capacity_10(u, &last_lba, &bs);
  if (rc != 0) {
    LOG_ERR("[UAS] READ_CAPACITY falló: %d", rc);
    goto fail;
  }
  u->num_sectors = last_lba + 1;
  u->sector_size = bs;

  block_device_t *bdev = kzalloc(sizeof(*bdev));
  if (!bdev) {
    rc = -ENOMEM;
    goto fail;
  }
  strcpy(bdev->name, u->name);
  bdev->num_sectors = u->num_sectors;
  bdev->sector_size = u->sector_size;
  bdev->is_read_only = is_cdrom ? 1 : 0;
  bdev->ops = &uas_ops;
  bdev->private_data = u;
  u->bdev = bdev;

  rc = blk_register(bdev);
  if (rc < 0) {
    kfree(bdev);
    goto fail;
  }

  rc = uas_spawn_worker(u);
  if (rc != 0) {
    blk_unregister(bdev);
    kfree(bdev);
    goto fail;
  }

  LOG_INFO("[UAS] %s: '%s %s' rev '%s' %u sectores de %u B (%u MB)", u->name,
           u->inquiry_vendor, u->inquiry_product, u->inquiry_revision,
           u->num_sectors, u->sector_size,
           u->num_sectors / (1024 * 1024 / u->sector_size));
  return 1;

fail:
  if (u->data_buf.virt)
    usb_dma_free(&u->data_buf);
  if (u->status_buf.virt)
    usb_dma_free(&u->status_buf);
  if (u->cmd_buf.virt)
    usb_dma_free(&u->cmd_buf);
  kfree(u);
  return rc;
}