// kernel/ata_pio.c
//
// Driver ATA completo (PIO, LBA28 + LBA48).
//
// Referencia: ATA/ATAPI-7, Intel PIIX3 datasheet, OSDev wiki.
//
// Arquitectura por capas:
//   ata_submit       → dispatcher desde el block layer
//   ata_read/write   → eligen LBA28 vs LBA48, gestionan reintentos
//   ata_issue_cmd_*  → construyen los registros con timing correcto
//   ata_pio_read/write → transfieren datos
//   ata_wait_*       → polling con timeout
//   ata_select_drive → selección de master/slave
//   ata_soft_reset   → reset del canal

#include "ata_pio.h"
#include "block.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"

// ===========================================================================
// Constantes
// ===========================================================================

// Puertos de los canales ATA.
#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_PRIMARY_IRQ 14
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376
#define ATA_SECONDARY_IRQ 15

// Registros (offset desde io_base).
#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_FEATURES 1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_DRIVE 6
#define ATA_REG_STATUS 7
#define ATA_REG_COMMAND 7

// Registro de control (offset desde ctrl_base).
#define ATA_CTRL_ALT_STATUS 0
#define ATA_CTRL_DEV_CTRL 2

// Bits del status.
#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF 0x20
#define ATA_SR_DSC 0x10
#define ATA_SR_DRQ 0x08
#define ATA_SR_CORR 0x04
#define ATA_SR_IDX 0x02
#define ATA_SR_ERR 0x01

// Bits del registro Error.
#define ATA_ER_ABRT 0x04
#define ATA_ER_UNC 0x40
#define ATA_ER_IDNF 0x10
#define ATA_ER_MC 0x20

// Comandos.
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_READ_SECTORS_EXT 0x24
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_SET_FEATURES 0xEF

// Features para SET FEATURES.
#define ATA_FEATURE_TRANSFER_MODE 0x03

// Bits del registro Drive/Head.
#define ATA_DRIVE_MASTER 0xA0
#define ATA_DRIVE_SLAVE 0xB0
#define ATA_DRIVE_LBA 0x40

// Bits del Device Control.
#define ATA_DEVCTRL_SRST 0x04
#define ATA_DEVCTRL_NIEN 0x02

// Timeout de polling (5 segundos).
#define ATA_TIMEOUT_SECONDS 5

// Reintentos por operación.
#define ATA_MAX_RETRIES 3

// ===========================================================================
// Tipos
// ===========================================================================

// Códigos de error semánticos.
enum ata_err {
  ATA_OK = 0,
  ATA_ERR_ABRT,    // comando abortado
  ATA_ERR_UNC,     // error de datos
  ATA_ERR_IDNF,    // sector no encontrado
  ATA_ERR_MC,      // media change
  ATA_ERR_DF,      // device fault
  ATA_ERR_BSY,     // timeout en BSY
  ATA_ERR_DRQ,     // timeout en DRQ
  ATA_ERR_TIMEOUT, // timeout general
  ATA_ERR_UNKNOWN,
  ATA_ERR_NODEV, // no hay disco
};

// Un canal IDE.
typedef struct ata_channel {
  uint16_t io_base;
  uint16_t ctrl_base;
  uint8_t irq;
  const char *name; // "primario" o "secundario"
  spinlock_t lock;  // protege operaciones del canal
  struct ata_device *master;
  struct ata_device *slave;
} ata_channel_t;

// Un disco ATA.
typedef struct ata_device {
  // Identidad
  char model[41];
  char serial[21];
  char firmware[9];

  // Ubicación
  ata_channel_t *channel;
  uint8_t drive; // 0=master, 1=slave

  // Tipo
  uint8_t is_atapi;

  // Capacidades detectadas por IDENTIFY
  uint8_t supports_lba;
  uint8_t supports_lba48;
  uint8_t supports_dma;
  uint8_t supports_flush;
  uint8_t supports_ncq;
  uint16_t pio_modes;   // bits 0-2: PIO 0-2, bit 3: PIO 3, bit 4: PIO 4
  uint16_t dma_modes;   // Multiword DMA 0-2
  uint16_t udma_modes;  // Ultra DMA 0-6
  uint32_t sector_size; // 512 o 4096
  uint64_t lba28_sectors;
  uint64_t lba48_sectors;

  // Modo activo
  uint8_t use_lba48;
  uint8_t pio_mode; // 0-4

  // Estado en runtime
  uint64_t num_sectors;
  uint32_t error_count;
  uint32_t retry_count;

  // Enlace al block layer
  block_device_t *bdev;

  // Lista para debug
  struct ata_device *next;
} ata_device_t;

// ===========================================================================
// Estado global
// ===========================================================================
static ata_channel_t g_channels[2];
static ata_device_t *g_devices = NULL; // lista para debug
static int g_device_count = 0;

// ===========================================================================
// Helpers de bajo nivel
// ===========================================================================

// Delay de 400 ns leyendo el alternate status 4 veces.
static inline void ata_io_delay(uint16_t ctrl_base) {
  inb(ctrl_base + ATA_CTRL_ALT_STATUS);
  inb(ctrl_base + ATA_CTRL_ALT_STATUS);
  inb(ctrl_base + ATA_CTRL_ALT_STATUS);
  inb(ctrl_base + ATA_CTRL_ALT_STATUS);
}

// Calcula el número de iteraciones de polling para ATA_TIMEOUT_SECONDS.
static uint64_t ata_timeout_iters(void) {
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    return 1000000000ULL; // ~1s conservador
  return freq * ATA_TIMEOUT_SECONDS;
}

// Espera a que BSY=0. Devuelve 0 o ATA_ERR_BSY.
static int ata_wait_not_busy(uint16_t io_base, uint64_t max_iters) {
  for (uint64_t i = 0; i < max_iters; i++) {
    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if ((status & ATA_SR_BSY) == 0)
      return 0;
    __asm__ volatile("pause");
  }
  return ATA_ERR_BSY;
}

// Espera a BSY=0 y DRQ=1. Devuelve 0 o error.
static int ata_wait_drq(uint16_t io_base, uint64_t max_iters) {
  for (uint64_t i = 0; i < max_iters; i++) {
    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (status & ATA_SR_BSY) {
      __asm__ volatile("pause");
      continue;
    }
    if (status & ATA_SR_ERR) {
      uint8_t err = inb(io_base + ATA_REG_ERROR);
      if (err & ATA_ER_ABRT)
        return ATA_ERR_ABRT;
      if (err & ATA_ER_UNC)
        return ATA_ERR_UNC;
      if (err & ATA_ER_IDNF)
        return ATA_ERR_IDNF;
      if (err & ATA_ER_MC)
        return ATA_ERR_MC;
      return ATA_ERR_UNKNOWN;
    }
    if (status & ATA_SR_DF)
      return ATA_ERR_DF;
    if (status & ATA_SR_DRQ)
      return 0;
    __asm__ volatile("pause");
  }
  return ATA_ERR_DRQ;
}

// Selecciona un drive (master/slave) y espera 400 ns.
static void ata_select_drive(ata_device_t *dev) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint8_t val = dev->drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER;
  outb(io + ATA_REG_DRIVE, val);
  ata_io_delay(ctrl);
}

// Soft reset del canal. Afecta a ambos discos.
static int ata_soft_reset(ata_channel_t *ch) {
  uint16_t ctrl = ch->ctrl_base;
  uint16_t io = ch->io_base;
  uint64_t timeout = ata_timeout_iters();

  // Escribir SRST=1 en Device Control.
  outb(ctrl + ATA_CTRL_DEV_CTRL, ATA_DEVCTRL_SRST);
  // Esperar 5 µs (mínimo del estándar).
  for (volatile int i = 0; i < 1000; i++) {
    __asm__ volatile("pause");
  }
  // Limpiar SRST.
  outb(ctrl + ATA_CTRL_DEV_CTRL, 0);
  // Esperar a que BSY=0.
  return ata_wait_not_busy(io, timeout);
}

// SET FEATURES: activa un modo de transferencia.
static int ata_set_features(ata_device_t *dev, uint8_t feature, uint8_t value) {
  uint16_t io = dev->channel->io_base;
  uint64_t timeout = ata_timeout_iters();

  int rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  ata_select_drive(dev);
  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  outb(io + ATA_REG_FEATURES, feature);
  outb(io + ATA_REG_SECCOUNT, value);
  outb(io + ATA_REG_COMMAND, ATA_CMD_SET_FEATURES);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status & ATA_SR_ERR)
    return ATA_ERR_UNKNOWN;
  return ATA_OK;
}

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
  // Strings.
  ata_copy_string(dev->model, &id[27], 20, sizeof(dev->model));
  ata_copy_string(dev->serial, &id[10], 10, sizeof(dev->serial));
  ata_copy_string(dev->firmware, &id[23], 4, sizeof(dev->firmware));

  // Capacidades generales (palabra 49).
  //   bit 8:  DMA soportado
  //   bit 9:  LBA soportado
  uint16_t caps = id[49];
  dev->supports_dma = (caps & (1 << 8)) ? 1 : 0;
  dev->supports_lba = (caps & (1 << 9)) ? 1 : 0;

  // Capacidades extendidas (palabra 83).
  //   bit 10: LBA48 soportado
  //   bit 12: FLUSH CACHE soportado
  uint16_t caps83 = id[83];
  dev->supports_lba48 = (caps83 & (1 << 10)) ? 1 : 0;
  dev->supports_flush = (caps83 & (1 << 12)) ? 1 : 0;

  // PIO modes (palabras 64 y 65).
  //   Palabra 64 bits 0-1: PIO modes 3-4 soportados.
  //   Palabra 65 bits 0-1: PIO modes 0-2 soportados.
  uint16_t pio64 = id[64];
  uint16_t pio65 = id[65];
  dev->pio_modes = 0;
  if (pio65 & (1 << 0))
    dev->pio_modes |= (1 << 0); // PIO 0
  if (pio65 & (1 << 1))
    dev->pio_modes |= (1 << 1); // PIO 1
  if (pio64 & (1 << 0))
    dev->pio_modes |= (1 << 2); // PIO 2
  if (pio64 & (1 << 1))
    dev->pio_modes |= (1 << 3); // PIO 3
  if (pio64 & (1 << 2))
    dev->pio_modes |= (1 << 4); // PIO 4

  // Multiword DMA (palabra 63).
  dev->dma_modes = id[63] & 0x07;

  // Ultra DMA (palabra 88).
  dev->udma_modes = id[88] & 0x7F;

  // NCQ (palabra 76 bit 8).
  if (id[76] & (1 << 8))
    dev->supports_ncq = 1;

  // Tamaños.
  dev->lba28_sectors = ata_words_to_u32(&id[60]);
  dev->lba48_sectors = ata_words_to_u64(&id[100]);

  // Palabra 106: sector size.
  //   Bits 15-12: exponente del sector físico.
  //   Bits 3-0:   exponente del sector lógico.
  // El tamaño es 2^exponente * 2 bytes (porque se cuenta en palabras de 16
  // bits).
  uint16_t word106 = id[106];
  uint16_t logical_words = word106 & 0x0F;
  if (logical_words == 0) {
    dev->sector_size = 512; // por defecto
  } else {
    dev->sector_size = 1u << (logical_words + 1);
  }
}

// ===========================================================================
// IDENTIFY
// ===========================================================================

// Intenta IDENTIFY DEVICE. Devuelve ATA_OK, ATA_ERR_NODEV, o error.
static int ata_identify(ata_device_t *dev) {
  uint16_t io = dev->channel->io_base;
  uint64_t timeout = ata_timeout_iters();

  // 1. Seleccionar el disco.
  ata_select_drive(dev);

  // 2. Leer status. Si es 0, no hay disco.
  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status == 0)
    return ATA_ERR_NODEV;

  // 3. Esperar BSY=0.
  int rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // 4. Resetear registros.
  outb(io + ATA_REG_SECCOUNT, 0);
  outb(io + ATA_REG_LBA0, 0);
  outb(io + ATA_REG_LBA1, 0);
  outb(io + ATA_REG_LBA2, 0);

  // 5. Enviar IDENTIFY DEVICE.
  outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

  // 6. Leer status.
  status = inb(io + ATA_REG_STATUS);
  if (status == 0)
    return ATA_ERR_NODEV;

  // 7. Esperar BSY=0.
  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // 8. Leer LBA1 y LBA2 para detectar ATAPI.
  uint8_t lba1 = inb(io + ATA_REG_LBA1);
  uint8_t lba2 = inb(io + ATA_REG_LBA2);

  if (lba1 != 0 || lba2 != 0) {
    // Es ATAPI. Drenar buffer y salir.
    dev->is_atapi = 1;
    if (ata_wait_drq(io, timeout) == 0) {
      for (int i = 0; i < 256; i++)
        (void)inw(io + ATA_REG_DATA);
    }
    return ATA_ERR_NODEV;
  }

  // 9. Esperar DRQ=1.
  rc = ata_wait_drq(io, timeout);
  if (rc != 0)
    return rc;

  // 10. Leer 256 palabras.
  uint16_t id[256];
  for (int i = 0; i < 256; i++)
    id[i] = inw(io + ATA_REG_DATA);

  // 11. Parsear.
  ata_parse_identify(dev, id);

  return ATA_OK;
}

// ===========================================================================
// Selección de modo
// ===========================================================================

// Elige LBA48 vs LBA28 y PIO mode. Devuelve ATA_OK o error.
static int ata_select_mode(ata_device_t *dev) {
  // LBA48 si está soportado y hay sectores.
  if (dev->supports_lba48 && dev->lba48_sectors > 0) {
    dev->use_lba48 = 1;
    dev->num_sectors = dev->lba48_sectors;
  } else if (dev->supports_lba && dev->lba28_sectors > 0) {
    dev->use_lba48 = 0;
    dev->num_sectors = dev->lba28_sectors;
  } else {
    LOG_WARN("[ATA] %s: ni LBA28 ni LBA48 soportados", dev->model);
    return ATA_ERR_UNKNOWN;
  }

  // Elegir el mejor PIO mode.
  // pio_modes bit 4 = PIO 4, bit 3 = PIO 3, etc.
  // Preferimos el más alto.
  dev->pio_mode = 0;
  if (dev->pio_modes & (1 << 4))
    dev->pio_mode = 4;
  else if (dev->pio_modes & (1 << 3))
    dev->pio_mode = 3;
  else if (dev->pio_modes & (1 << 2))
    dev->pio_mode = 2;
  else if (dev->pio_modes & (1 << 1))
    dev->pio_mode = 1;
  else
    dev->pio_mode = 0;

  // Activar el PIO mode con SET FEATURES.
  // El valor para PIO mode N es N (0-4).
  // Los modos PIO 3 y 4 requieren bit 3 del valor.
  uint8_t pio_val = dev->pio_mode;
  if (dev->pio_mode >= 3)
    pio_val = dev->pio_mode - 3 + 0x08; // PIO 3 → 0x08, PIO 4 → 0x09

  int rc = ata_set_features(dev, ATA_FEATURE_TRANSFER_MODE, pio_val);
  if (rc != 0) {
    LOG_WARN("[ATA] %s: SET FEATURES PIO %u falló (rc=%d)", dev->model,
             dev->pio_mode, rc);
    // No es fatal: seguimos con PIO 0 por defecto.
    dev->pio_mode = 0;
  }

  return ATA_OK;
}

// ===========================================================================
// Emisión de comandos
// ===========================================================================

// Prepara los registros para LBA48 y envía el comando.
// Timing estricto: wait_not_busy entre cada escritura.
static int ata_issue_cmd_lba48(ata_device_t *dev, uint64_t lba, uint32_t count,
                               uint8_t command) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  // 1. Esperar BSY=0.
  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // 2. Drive/Head. En LBA48 solo lleva drive, sin bits de LBA.
  uint8_t drive_head = ATA_DRIVE_LBA | (dev->drive << 4);
  outb(io + ATA_REG_DRIVE, drive_head);
  ata_io_delay(ctrl);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // 3. Escribir los 3 bytes altos (LBA3-5) y el byte alto del count.
  outb(io + ATA_REG_SECCOUNT, (uint8_t)(count >> 8));
  outb(io + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
  outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 32) & 0xFF));
  outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 40) & 0xFF));
  ata_io_delay(ctrl);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // 4. Escribir los 3 bytes bajos (LBA0-2) y el byte bajo del count.
  outb(io + ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));
  outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
  outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
  outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
  ata_io_delay(ctrl);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // 5. Enviar comando.
  outb(io + ATA_REG_COMMAND, command);

  return ATA_OK;
}

// Prepara los registros para LBA28 y envía el comando.
static int ata_issue_cmd_lba28(ata_device_t *dev, uint64_t lba, uint32_t count,
                               uint8_t command) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // Drive/Head: 0xE0 = LBA mode + master. Los bits 0-3 llevan LBA[24:27].
  uint8_t drive_head = 0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F);
  outb(io + ATA_REG_DRIVE, drive_head);
  ata_io_delay(ctrl);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // Sector count.
  if (count > 255)
    count = 255;
  outb(io + ATA_REG_SECCOUNT, (uint8_t)count);
  ata_io_delay(ctrl);

  // LBA Low/Mid/High.
  outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
  ata_io_delay(ctrl);
  outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
  ata_io_delay(ctrl);
  outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
  ata_io_delay(ctrl);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // Comando.
  outb(io + ATA_REG_COMMAND, command);

  return ATA_OK;
}

// ===========================================================================
// Transferencia de datos (PIO)
// ===========================================================================

// Transfiere `count` sectores desde/hacia el disco.
// dir = 0 (read) o 1 (write).
static int ata_pio_transfer(ata_device_t *dev, uint32_t count, void *buf,
                            int dir) {
  uint16_t io = dev->channel->io_base;
  uint64_t timeout = ata_timeout_iters();
  uint16_t *ptr = (uint16_t *)buf;

  uint32_t words_per_sector = dev->sector_size / 2;

  for (uint32_t s = 0; s < count; s++) {
    int rc = ata_wait_drq(io, timeout);
    if (rc != 0)
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
// Operaciones de alto nivel (read/write/flush)
// ===========================================================================

// Lee `count` sectores. Elige LBA48 o LBA28. Divide en chunks si hace falta.
static int ata_read(ata_device_t *dev, uint64_t lba, uint32_t count,
                    void *buf) {
  uint8_t *ptr = (uint8_t *)buf;

  if (dev->use_lba48) {
    // LBA48: count hasta 65536.
    while (count > 0) {
      uint32_t chunk = count > 65536 ? 65536 : count;
      int rc = ata_issue_cmd_lba48(dev, lba, chunk, ATA_CMD_READ_SECTORS_EXT);
      if (rc != 0)
        return rc;
      rc = ata_pio_transfer(dev, chunk, ptr, 0);
      if (rc != 0)
        return rc;
      lba += chunk;
      ptr += chunk * 512;
      count -= chunk;
    }
  } else {
    // LBA28: count hasta 255.
    while (count > 0) {
      uint32_t chunk = count > 255 ? 255 : count;
      int rc = ata_issue_cmd_lba28(dev, lba, chunk, ATA_CMD_READ_SECTORS);
      if (rc != 0)
        return rc;
      rc = ata_pio_transfer(dev, chunk, ptr, 0);
      if (rc != 0)
        return rc;
      lba += chunk;
      ptr += chunk * 512;
      count -= chunk;
    }
  }

  return ATA_OK;
}

// Escribe `count` sectores.
static int ata_write(ata_device_t *dev, uint64_t lba, uint32_t count,
                     const void *buf) {
  const uint8_t *ptr = (const uint8_t *)buf;

  if (dev->use_lba48) {
    while (count > 0) {
      uint32_t chunk = count > 65536 ? 65536 : count;
      int rc = ata_issue_cmd_lba48(dev, lba, chunk, ATA_CMD_WRITE_SECTORS_EXT);
      if (rc != 0)
        return rc;
      rc = ata_pio_transfer(dev, chunk, (void *)ptr, 1);
      if (rc != 0)
        return rc;
      lba += chunk;
      ptr += chunk * 512;
      count -= chunk;
    }
  } else {
    while (count > 0) {
      uint32_t chunk = count > 255 ? 255 : count;
      int rc = ata_issue_cmd_lba28(dev, lba, chunk, ATA_CMD_WRITE_SECTORS);
      if (rc != 0)
        return rc;
      rc = ata_pio_transfer(dev, chunk, (void *)ptr, 1);
      if (rc != 0)
        return rc;
      lba += chunk;
      ptr += chunk * 512;
      count -= chunk;
    }
  }

  return ATA_OK;
}

// FLUSH CACHE.
static int ata_flush(ata_device_t *dev) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint64_t timeout = ata_timeout_iters();

  int rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  ata_select_drive(dev);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  // FLUSH CACHE EXT si LBA48, si no FLUSH CACHE.
  uint8_t cmd =
      dev->supports_flush ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE;
  outb(io + ATA_REG_COMMAND, cmd);
  (void)ctrl;

  rc = ata_wait_not_busy(io, timeout);
  if (rc != 0)
    return rc;

  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status & ATA_SR_ERR)
    return ATA_ERR_UNKNOWN;

  return ATA_OK;
}

// Wrapper con reintentos y reset de canal.
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

    // Si es ABRT y tenemos LBA48, probar fallback a LBA28.
    if (rc == ATA_ERR_ABRT && dev->use_lba48 && dev->supports_lba &&
        dev->lba28_sectors > 0 && lba < dev->lba28_sectors) {
      LOG_WARN("[ATA] %s: ABRT en LBA48, probando LBA28 (lba=%lu)", dev->model,
               (unsigned long)lba);
      dev->use_lba48 = 0;
      dev->num_sectors = dev->lba28_sectors;
      dev->retry_count++;
      continue;
    }

    // Otros errores: reset del canal y reintentar.
    LOG_WARN("[ATA] %s: error %d, reset de canal (intento %d/%d)", dev->model,
             rc, attempt + 1, ATA_MAX_RETRIES);
    ata_soft_reset(dev->channel);
    ata_select_drive(dev);
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
  LOG_INFO("    DMA: %s (modos 0x%x)", dev->supports_dma ? "sí" : "no",
           dev->dma_modes);
  LOG_INFO("    UDMA: modos 0x%x", dev->udma_modes);
  LOG_INFO("    NCQ: %s", dev->supports_ncq ? "sí" : "no");
  LOG_INFO("    errores: %u, reintentos: %u", dev->error_count,
           dev->retry_count);
}

static block_ops_t ata_pio_ops = {
    .submit = ata_submit,
    .flush = ata_submit_flush,
    .dump = ata_submit_dump,
};

// ===========================================================================
// Registro en el block layer
// ===========================================================================

static const char *ata_name_for(ata_channel_t *ch, uint8_t drive) {
  if (ch->io_base == ATA_PRIMARY_IO)
    return drive ? "hdb" : "hda";
  return drive ? "hdd" : "hdc";
}

static int ata_register_device(ata_device_t *dev, const char *name) {
  block_device_t *bdev = (block_device_t *)kzalloc(sizeof(block_device_t));
  if (!bdev) {
    LOG_ERR("[ATA] kzalloc(block_device_t) falló");
    return -ENOMEM;
  }

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

  int rc = blk_register(bdev);
  if (rc < 0) {
    kfree(bdev);
    return rc;
  }

  dev->bdev = bdev;
  return 0;
}

// ===========================================================================
// Detección e inicialización
// ===========================================================================

static void ata_probe_device(ata_channel_t *ch, uint8_t drive,
                             const char *channel_name) {
  ata_device_t *dev = (ata_device_t *)kzalloc(sizeof(ata_device_t));
  if (!dev) {
    LOG_ERR("[ATA] kzalloc(ata_device_t) falló");
    return;
  }
  dev->channel = ch;
  dev->drive = drive;

  int rc = ata_identify(dev);
  const char *drive_name = drive ? "slave" : "master";

  if (rc == ATA_OK) {
    // Disco ATA detectado. Seleccionar modo.
    if (ata_select_mode(dev) != ATA_OK) {
      LOG_ERR("[ATA] %s %s: no se pudo seleccionar modo", channel_name,
              drive_name);
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
      LOG_ERR("[ATA] no se pudo registrar %s", name);
      kfree(dev);
      return;
    }

    if (drive == 0)
      ch->master = dev;
    else
      ch->slave = dev;

    dev->next = g_devices;
    g_devices = dev;
    g_device_count++;
  } else if (dev->is_atapi) {
    LOG_INFO("[ATA] ATAPI detectado en %s %s (no registrado en Fase 1)",
             channel_name, drive_name);
    kfree(dev);
  } else if (rc == ATA_ERR_NODEV) {
    // Slot vacío. Normal.
    kfree(dev);
  } else {
    LOG_WARN("[ATA] error identificando %s %s (rc=%d)", channel_name,
             drive_name, rc);
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

int ata_pio_init(void) {
  LOG_INFO("[ATA] Iniciando detección de discos PATA...");

  ata_init_channel(&g_channels[0], ATA_PRIMARY_IO, ATA_PRIMARY_CTRL,
                   ATA_PRIMARY_IRQ, "primario");
  ata_init_channel(&g_channels[1], ATA_SECONDARY_IO, ATA_SECONDARY_CTRL,
                   ATA_SECONDARY_IRQ, "secundario");

  // Detectar discos.
  ata_probe_device(&g_channels[0], 0, "primario");
  ata_probe_device(&g_channels[0], 1, "primario");
  ata_probe_device(&g_channels[1], 0, "secundario");
  ata_probe_device(&g_channels[1], 1, "secundario");

  LOG_INFO("[ATA] Detección completada (%d discos)", g_device_count);
  return 0;
}

// ===========================================================================
// Debug
// ===========================================================================
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