// kernel/ata_pio.c
//
// Driver ATA PIO. Fase 1: detección de discos con IDENTIFY DEVICE.
//
// En esta parte solo detectamos discos y los registramos en el block layer.
// La lectura/escritura (READ SECTORS / WRITE SECTORS) se implementa en la
// parte 2.
//
// Referencia: ATA/ATAPI-7, Intel PIIX3 datasheet, OSDev wiki.

#include "ata_pio.h"
#include "block.h"
#include "io.h"
#include "klog.h"
#include "string.h"
#include "heap.h"
#include "uaccess.h"

// ---------------------------------------------------------------------------
// Puertos de los canales ATA.
// ---------------------------------------------------------------------------
#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

// Registros (offset desde io_base).
#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1     // lectura
#define ATA_REG_FEATURES 1  // escritura
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_DRIVE 6
#define ATA_REG_STATUS 7    // lectura
#define ATA_REG_COMMAND 7   // escritura

// Bits del status.
#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF 0x20
#define ATA_SR_DSC 0x10
#define ATA_SR_DRQ 0x08
#define ATA_SR_CORR 0x04
#define ATA_SR_IDX 0x02
#define ATA_SR_ERR 0x01

// Comandos.
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1

// Bits del registro Drive/Head.
#define ATA_DRIVE_MASTER 0xA0
#define ATA_DRIVE_SLAVE 0xB0

// ---------------------------------------------------------------------------
// Timeout de polling.
//
// 5 segundos expresados en iteraciones de un bucle con "pause". Usamos el
// TSC calibrado por klog para convertirlo. Si no está calibrado, usamos
// un valor conservador de ~1e9 iteraciones.
// ---------------------------------------------------------------------------
#define ATA_TIMEOUT_SECONDS 5

// ---------------------------------------------------------------------------
// Estado de un disco.
// ---------------------------------------------------------------------------
typedef struct {
  uint16_t io_base;
  uint16_t ctrl_base;
  uint8_t drive; // 0 = master, 1 = slave
  uint8_t is_atapi;

  uint64_t num_sectors;
  uint32_t sector_size; // 512 para ATA
  char model[41];       // NUL-terminated
  char serial[21];      // NUL-terminated
  char firmware[9];     // NUL-terminated

  uint8_t supports_lba48;
} ata_device_t;

// ---------------------------------------------------------------------------
// Helpers de bajo nivel
// ---------------------------------------------------------------------------

// Espera 400 ns leyendo el alternate status 4 veces.
// En hardware real cada inb tarda ~100 ns. En QEMU es instantáneo, pero
// el estándar ATA lo requiere para que el disco seleccionado se estabilice.
static inline void ata_io_delay(uint16_t ctrl_base) {
  inb(ctrl_base + 0);
  inb(ctrl_base + 0);
  inb(ctrl_base + 0);
  inb(ctrl_base + 0);
}

// Espera hasta que BSY=0. Devuelve 0 si OK, -ETIMEDOUT si se agota.
static int ata_wait_not_busy(uint16_t io_base, uint64_t max_iters) {
  for (uint64_t i = 0; i < max_iters; i++) {
    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if ((status & ATA_SR_BSY) == 0)
      return 0;
    __asm__ volatile("pause");
  }
  return -ETIMEDOUT;
}

// Espera hasta que BSY=0 y DRQ=1. Devuelve 0 si OK, negativo si error.
static int ata_wait_drq(uint16_t io_base, uint64_t max_iters) {
  for (uint64_t i = 0; i < max_iters; i++) {
    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (status & ATA_SR_BSY) {
      __asm__ volatile("pause");
      continue;
    }
    if (status & ATA_SR_ERR)
      return -EIO;
    if (status & ATA_SR_DF)
      return -EIO;
    if (status & ATA_SR_DRQ)
      return 0;
    __asm__ volatile("pause");
  }
  return -ETIMEDOUT;
}

// Calcula el número de iteraciones de polling para ATA_TIMEOUT_SECONDS.
// Usa el TSC si está calibrado. Si no, un valor conservador.
static uint64_t ata_timeout_iters(void) {
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0) {
    // Sin TSC calibrado. 1e9 iteraciones con "pause" tarda ~1s en un
    // CPU moderno. Es un límite holgado.
    return 1000000000ULL;
  }
  // Cada iteración del bucle hace un inb (~1 µs en hardware real,
  // instantáneo en QEMU) + un pause. En la práctica el bucle va a
  // mucha más frecuencia que el TSC, así que usar el TSC como
  // referencia da un margen enorme. Es lo que queremos: preferimos
  // que un disco lento tarde en ser detectado que dar un falso timeout.
  return freq * ATA_TIMEOUT_SECONDS;
}

// Selecciona un disco y espera 400 ns.
static void ata_select_drive(uint16_t io_base, uint16_t ctrl_base,
                             uint8_t drive) {
  outb(io_base + ATA_REG_DRIVE, drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER);
  ata_io_delay(ctrl_base);
}

// ---------------------------------------------------------------------------
// Parseo de la respuesta de IDENTIFY DEVICE.
//
// La respuesta son 256 palabras (512 bytes). Los strings vienen con los
// bytes de cada palabra en orden big-endian (network order), hay que
// intercambiarlos para obtener ASCII legible.
// ---------------------------------------------------------------------------

// Copia `words` palabras (2 bytes cada una) a `dst`, byte-swapping y
// trimmeando espacios finales. `dst` es NUL-terminated.
static void ata_copy_string(char *dst, const uint16_t *words, int num_words,
                            size_t dst_size) {
  size_t j = 0;
  for (int i = 0; i < num_words; i++) {
    char hi = (char)(words[i] >> 8);
    char lo = (char)(words[i] & 0xFF);
    if (j + 1 < dst_size) {
      dst[j++] = hi;
    }
    if (j + 1 < dst_size) {
      dst[j++] = lo;
    }
  }
  dst[j] = '\0';

  // Trimear espacios finales.
  while (j > 0 && (dst[j - 1] == ' ' || dst[j - 1] == '\0')) {
    dst[--j] = '\0';
  }
}

static uint32_t ata_words_to_u32(const uint16_t *words) {
  // Little-endian: palabra baja primero, luego palabra alta.
  return ((uint32_t)words[1] << 16) | (uint32_t)words[0];
}

static uint64_t ata_words_to_u64(const uint16_t *words) {
  // Little-endian en 4 palabras: la 0 es la más baja.
  return ((uint64_t)words[3] << 48) | ((uint64_t)words[2] << 32) |
         ((uint64_t)words[1] << 16) | (uint64_t)words[0];
}

static void ata_parse_identify(ata_device_t *dev, const uint16_t *id) {
  // Modelo: palabras 27-46 (20 palabras = 40 bytes).
  ata_copy_string(dev->model, &id[27], 20, sizeof(dev->model));

  // Serial: palabras 10-19 (10 palabras = 20 bytes).
  ata_copy_string(dev->serial, &id[10], 10, sizeof(dev->serial));

  // Firmware: palabras 23-26 (4 palabras = 8 bytes).
  ata_copy_string(dev->firmware, &id[23], 4, sizeof(dev->firmware));

  // Capacidades: palabra 83.
  //   Bit 10: soporta LBA48.
  //   Bit 9:  soporta DMA.
  dev->supports_lba48 = (id[83] & (1 << 10)) ? 1 : 0;

  // Número de sectores:
  //   Si LBA48, palabras 100-103 (64 bits).
  //   Si LBA28, palabras 60-61 (32 bits).
  uint32_t lba28 = ata_words_to_u32(&id[60]);
  uint64_t lba48 = ata_words_to_u64(&id[100]);

  if (dev->supports_lba48 && lba48 > 0) {
    dev->num_sectors = lba48;
  } else if (lba28 > 0) {
    dev->num_sectors = lba28;
  } else {
    dev->num_sectors = 0;
  }

  dev->sector_size = 512;
}

// ---------------------------------------------------------------------------
// Detección de un disco.
//
// Devuelve 0 si se detectó un disco ATA y se rellenó `dev`.
// Devuelve -ENODEV si no hay disco o es ATAPI.
// Devuelve -EIO si hubo error de comunicación.
// ---------------------------------------------------------------------------
static int ata_identify(ata_device_t *dev, uint16_t io_base,
                        uint16_t ctrl_base, uint8_t drive) {
  memset(dev, 0, sizeof(*dev));
  dev->io_base = io_base;
  dev->ctrl_base = ctrl_base;
  dev->drive = drive;

  uint64_t timeout = ata_timeout_iters();

  // 1. Seleccionar el disco.
  ata_select_drive(io_base, ctrl_base, drive);

  // 2. Comprobar que hay algo. Leer el status: si es 0, no hay disco.
  uint8_t status = inb(io_base + ATA_REG_STATUS);
  if (status == 0) {
    return -ENODEV;
  }

  // 3. Esperar a que BSY=0.
  int rc = ata_wait_not_busy(io_base, timeout);
  if (rc < 0)
    return rc;

  // 4. Resetear los registros de dirección.
  outb(io_base + ATA_REG_DRIVE, drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER);
  ata_io_delay(ctrl_base);
  outb(io_base + ATA_REG_SECCOUNT, 0);
  outb(io_base + ATA_REG_LBA0, 0);
  outb(io_base + ATA_REG_LBA1, 0);
  outb(io_base + ATA_REG_LBA2, 0);

  // 5. Enviar IDENTIFY DEVICE.
  outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

  // 6. Leer status. Si es 0, no hay disco (el comando no fue aceptado).
  status = inb(io_base + ATA_REG_STATUS);
  if (status == 0)
    return -ENODEV;

  // 7. Esperar a que BSY=0.
  rc = ata_wait_not_busy(io_base, timeout);
  if (rc < 0)
    return rc;

  // 8. Leer LBA Mid (LBA1) y LBA High (LBA2).
  //    - Si son 0, es un disco ATA.
  //    - Si son != 0, es un ATAPI (o SATA con bridge).
  uint8_t lba1 = inb(io_base + ATA_REG_LBA1);
  uint8_t lba2 = inb(io_base + ATA_REG_LBA2);
  if (lba1 != 0 || lba2 != 0) {
    // Podría ser ATAPI. Intentamos IDENTIFY PACKET DEVICE para
    // confirmarlo, pero por ahora lo marcamos como ATAPI y salimos.
    dev->is_atapi = 1;
    return -ENODEV; // En Fase 1 no registramos ATAPI.
  }

  // 9. Esperar a que DRQ=1 (el disco tiene datos listos).
  rc = ata_wait_drq(io_base, timeout);
  if (rc < 0)
    return rc;

  // 10. Leer 256 palabras (512 bytes).
  uint16_t id[256];
  for (int i = 0; i < 256; i++) {
    id[i] = inw(io_base + ATA_REG_DATA);
  }

  // 11. Parsear.
  ata_parse_identify(dev, id);

  if (dev->num_sectors == 0) {
    LOG_WARN("[ATA] IDENTIFY devolvió 0 sectores (¿disco no soportado?)");
    return -EIO;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// ops del block layer
//
// En Fase 1 (detección), submit devuelve -ENOSYS. La lectura/escritura
// se implementa en la parte 2.
// ---------------------------------------------------------------------------
static int ata_pio_submit(block_device_t *bdev, bio_t *bio) {
  (void)bdev;
  (void)bio;
  return -ENOSYS;
}

static void ata_pio_flush(block_device_t *bdev) {
  (void)bdev;
  // Nada por ahora.
}

static void ata_pio_dump_dev(block_device_t *bdev) {
  ata_device_t *dev = (ata_device_t *)bdev->private_data;
  if (!dev)
    return;
  LOG_INFO("    modelo: '%s'", dev->model);
  LOG_INFO("    serial: '%s'", dev->serial);
  LOG_INFO("    firmware: '%s'", dev->firmware);
  LOG_INFO("    LBA48: %s", dev->supports_lba48 ? "sí" : "no");
}

static block_ops_t ata_pio_ops = {
    .submit = ata_pio_submit,
    .flush = ata_pio_flush,
    .dump = ata_pio_dump_dev,
};

// ---------------------------------------------------------------------------
// Registro de un disco detectado en el block layer.
// ---------------------------------------------------------------------------
static const char *ata_channel_name(uint16_t io_base, uint8_t drive) {
  // Primario: hda (master), hdb (slave).
  // Secundario: hdc (master), hdd (slave).
  if (io_base == ATA_PRIMARY_IO)
    return drive ? "hdb" : "hda";
  return drive ? "hdd" : "hdc";
}

static int ata_register_device(const ata_device_t *src, const char *name) {
  // 1. Copiar los datos del disco a un ata_device_t en heap.
  ata_device_t *dev = (ata_device_t *)kmalloc(sizeof(ata_device_t));
  if (!dev) {
    LOG_ERR("[ATA] kmalloc(ata_device_t) falló");
    return -ENOMEM;
  }
  memcpy(dev, src, sizeof(*dev));

  // 2. Crear el block_device_t.
  block_device_t *bdev = (block_device_t *)kzalloc(sizeof(block_device_t));
  if (!bdev) {
    LOG_ERR("[ATA] kzalloc(block_device_t) falló");
    kfree(dev);
    return -ENOMEM;
  }

  // 3. Rellenar.
  size_t i = 0;
  while (name[i] && i < sizeof(bdev->name) - 1) {
    bdev->name[i] = name[i];
    i++;
  }
  bdev->name[i] = '\0';

  bdev->num_sectors = dev->num_sectors;
  bdev->sector_size = dev->sector_size;
  bdev->is_read_only = 0;
  bdev->is_partition = 0;
  bdev->parent = NULL;
  bdev->start_lba = 0;
  bdev->ops = &ata_pio_ops;
  bdev->private_data = dev;

  // 4. Registrar.
  int rc = blk_register(bdev);
  if (rc < 0) {
    kfree(bdev);
    kfree(dev);
    return rc;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Init del driver: detecta discos en los 4 slots.
// ---------------------------------------------------------------------------
static void ata_probe_channel(uint16_t io_base, uint16_t ctrl_base,
                              const char *channel_name) {
  for (uint8_t drive = 0; drive < 2; drive++) {
    ata_device_t dev;
    int rc = ata_identify(&dev, io_base, ctrl_base, drive);
    const char *drive_name = drive ? "slave" : "master";

    if (rc == 0) {
      const char *name = ata_channel_name(io_base, drive);
      LOG_INFO("[ATA] Detectado %s en %s %s: '%s' (%lu sectores, %lu MB)",
               name, channel_name, drive_name, dev.model,
               (unsigned long)dev.num_sectors,
               (unsigned long)(dev.num_sectors * 512 / (1024 * 1024)));

      int rrc = ata_register_device(&dev, name);
      if (rrc < 0) {
        LOG_ERR("[ATA] No se pudo registrar %s (rc=%d)", name, rrc);
      }
    } else if (dev.is_atapi) {
      LOG_INFO("[ATA] ATAPI detectado en %s %s (no registrado en Fase 1)",
               channel_name, drive_name);
    } else if (rc == -ENODEV) {
      // No hay disco en este slot. Normal.
    } else {
      LOG_WARN("[ATA] Error al identificar %s %s (rc=%d)", channel_name,
               drive_name, rc);
    }
  }
}

int ata_pio_init(void) {
  LOG_INFO("[ATA] Iniciando detección de discos PATA...");

  ata_probe_channel(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, "primario");
  ata_probe_channel(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, "secundario");

  LOG_INFO("[ATA] Detección completada");
  return 0;
}

// ---------------------------------------------------------------------------
// Registro del driver
// ---------------------------------------------------------------------------
struct driver ata_pio_driver = {
    .name = "ata_pio",
    .init = ata_pio_init,
    .shutdown = NULL,
};