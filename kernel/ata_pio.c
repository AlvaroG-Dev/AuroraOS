// kernel/ata_pio.c
//
// Driver ATA PIO + DMA. Usa los helpers de ata_common.
//
// Lee/escribe con PIO o DMA (Bus Master IDE).
//
// [FIX] Detección en tres fases para permitir compatibilidad de par
// master/slave:
//   Fase A (ata_pio_init):    detecta los ATA, NO selecciona modo ni registra.
//   Fase B (atapi_init):      detecta los ATAPI y rellena ch->master/slave.
//   Fase C (ata_pio_finalize): selecciona modo (viendo al par) y registra.

#include "ata_pio.h"
#include "ata_common.h"
#include "ata_device.h"
#include "ata_dma.h"
#include "block.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "pci.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"

// ===========================================================================
// Estado global
// ===========================================================================
static ata_channel_t g_channels[2];
static ata_device_t *g_devices = NULL;
static int g_device_count = 0;

static int g_ide_present = 0;

// [FIX] Lista temporal de dispositivos detectados en Fase A, pendientes
// de que Fase C les asigne modo y los registre.
static ata_device_t *g_pending_devices = NULL;
static int g_pending_count = 0;

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

int ata_ide_present(void) { return g_ide_present; }

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
// IDENTIFY DEVICE.
//
// Clasificación por FIRMA, como hace libata (ata_dev_classify):
//
//   1. SRST del canal para dejar el bus en un estado conocido.
//   2. Seleccionar el drive.
//   3. Leer status. Si es 0x7F (VirtualBox slot vacío) o 0xFF (bus
//      flotante) => no hay dispositivo. 0x00 NO se filtra: un ATAPI en
//      reposo puede devolver 0x00 legítimamente.
//   4. Enviar IDENTIFY DEVICE (0xEC) sin importar el status.
//   5. Leer LBA1/LBA2 (la FIRMA):
//        - 0x00/0x00 => es ATA. Leer el IDENTIFY.
//        - 0x14/0xEB => es ATAPI. Drenar si DRQ=1, retornar NODEV.
//        - 0x3C/0xC3 => es SATA (port multiplier). Retornar NODEV.
//        - Otros     => firma desconocida. Retornar NODEV.
//
// El SRST se hace SIEMPRE, incluso si el status parece válido. Es
// imprescindible porque:
//   - Restaura la firma 0x14/0xEB en un ATAPI que la haya perdido.
//   - Limpia INTRQ residual del firmware UEFI (causa de tormentas de
//     IRQ en VirtualBox si nIEN=0 se activa sin reset previo).
//   - Pone el bus en un estado determinista, eliminando las diferencias
//     QEMU/VirtualBox.
// ===========================================================================
static int ata_identify(ata_device_t *dev) {
  uint16_t io = dev->channel->io_base;
  uint16_t ctrl = dev->channel->ctrl_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  LOG_INFO("[ATA] identify: io=0x%x drive=%u", io, dev->drive);

  // -------------------------------------------------------------------------
  // 1. SRST para limpiar el estado del firmware y de los drives.
  //
  // SRST resetea AMBOS drives del canal. Es seguro aquí porque estamos
  // en Fase A: ningún drive ha recibido SET FEATURES todavía, así que
  // ninguno pierde su modo.
  // -------------------------------------------------------------------------
  rc = ata_soft_reset(dev->channel);
  if (rc != ATA_OK) {
    LOG_WARN("[ATA]   SRST falló: %d", rc);
    return rc;
  }

  // -------------------------------------------------------------------------
  // 2. Seleccionar el drive.
  // -------------------------------------------------------------------------
  ata_select_drive(dev->channel, dev->drive);

  uint8_t status = inb(io + ATA_REG_STATUS);
  LOG_INFO("[ATA]   status tras SRST+select = 0x%02x", status);

  // -------------------------------------------------------------------------
  // 3. Filtrar SOLO los valores que indican "no hay dispositivo":
  //      0x7F = slot vacío en VirtualBox (todos los bits excepto BSY).
  //      0xFF = bus flotante (sin controlador o slot vacío en QEMU).
  //
  // NO filtramos 0x00: un ATAPI en reposo devuelve 0x00 legítimamente
  // (no activa DRDY hasta recibir un comando). Filtrarlo descartaba
  // los CD-ROM/DVD antes de poder clasificarlos.
  // -------------------------------------------------------------------------
  if (status == 0x7F || status == 0xFF) {
    LOG_INFO("[ATA]   slot vacío (status=0x%02x)", status);
    return ATA_ERR_NODEV;
  }

  // -------------------------------------------------------------------------
  // 4. Enviar IDENTIFY DEVICE con LBA1/LBA2 = 0.
  //
  // Enviar 0xEC a un ATAPI es seguro: responde con ABRT y deja la firma
  // 0x14/0xEB en LBA1/LBA2. Esa firma es la que usaremos para clasificar.
  // -------------------------------------------------------------------------
  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK) {
    LOG_WARN("[ATA]   wait_not_busy (pre-identify) = %d", rc);
    return rc;
  }

  outb(io + ATA_REG_SECCOUNT, 0);
  outb(io + ATA_REG_LBA0, 0);
  outb(io + ATA_REG_LBA1, 0);
  outb(io + ATA_REG_LBA2, 0);
  outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

  // -------------------------------------------------------------------------
  // 5. Leer status y esperar BSY=0. Si el status es 0 tras enviar el
  //    comando, el dispositivo no existe (el comando se perdió en el
  //    bus flotante).
  // -------------------------------------------------------------------------
  status = inb(io + ATA_REG_STATUS);
  LOG_INFO("[ATA]   status post-IDENTIFY = 0x%02x", status);
  if (status == 0x00) {
    LOG_INFO("[ATA]   comando perdido (bus flotante)");
    return ATA_ERR_NODEV;
  }

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK) {
    LOG_WARN("[ATA]   wait_not_busy (post-identify) = %d", rc);
    return rc;
  }

  // -------------------------------------------------------------------------
  // 6. Leer la FIRMA. Esta es la clasificación definitiva.
  // -------------------------------------------------------------------------
  uint8_t lba1 = inb(io + ATA_REG_LBA1);
  uint8_t lba2 = inb(io + ATA_REG_LBA2);
  LOG_INFO("[ATA]   firma lba1=0x%02x lba2=0x%02x", lba1, lba2);

  // Firma ATAPI: 0x14/0xEB.
  if (lba1 == 0x14 && lba2 == 0xEB) {
    LOG_INFO("[ATA]   es ATAPI");
    dev->is_atapi = 1;
    // Drenar el buffer si el ATAPI dejó DRQ=1. Algunos firmwares
    // responden al IDENTIFY DEVICE con datos basura antes del ABRT.
    if (ata_wait_drq(io, timeout) == ATA_OK) {
      for (int i = 0; i < 256; i++)
        (void)inw(io + ATA_REG_DATA);
    }
    return ATA_ERR_NODEV;
  }

  // Firma SATA: 0x3C/0xC3. No manejable por el driver PATA.
  if (lba1 == 0x3C && lba2 == 0xC3) {
    LOG_INFO("[ATA]   es SATA (no manejable por PATA)");
    dev->is_atapi = 1; // marcar como "no ATA" para que no se registre
    return ATA_ERR_NODEV;
  }

  // Firma desconocida. No manejable.
  if (lba1 != 0 || lba2 != 0) {
    LOG_INFO("[ATA]   firma desconocida, no manejable");
    dev->is_atapi = 1;
    return ATA_ERR_NODEV;
  }

  // -------------------------------------------------------------------------
  // 7. Firma 0x00/0x00 => es ATA. Esperar DRQ y leer el IDENTIFY.
  // -------------------------------------------------------------------------
  rc = ata_wait_drq(io, timeout);
  if (rc != ATA_OK) {
    LOG_WARN("[ATA]   wait_drq (identify) = %d", rc);
    return rc;
  }

  uint16_t id[256];
  for (int i = 0; i < 256; i++)
    id[i] = inw(io + ATA_REG_DATA);

  LOG_INFO("[ATA]   IDENTIFY leído OK");
  ata_parse_identify(dev, id);
  return ATA_OK;
}

// ===========================================================================
// Compatibilidad de modos master/slave.
//
// [FIX] Usa los flags master_is_atapi / slave_is_atapi del canal para saber
// el TIPO del peer sin castear el puntero a ata_device_t (los structs
// ata_device_t y atapi_device_t son incompatibles).
// ===========================================================================

// Devuelve el otro drive del mismo canal, o NULL si no hay.
// NOTA: solo devuelve peers que sean ata_device_t (es decir, ATA). Si el
// peer es ATAPI, devuelve NULL. Para saber si el peer es ATAPI, consulta
// los flags del canal.
ata_device_t *ata_dev_pair(ata_device_t *dev) {
  if (!dev || !dev->channel)
    return NULL;
  ata_channel_t *ch = dev->channel;

  if (dev->drive == 0) {
    // El peer es el slave. Solo lo devolvemos si NO es ATAPI.
    if (ch->slave_is_atapi)
      return NULL;
    return (ata_device_t *)ch->slave;
  } else {
    // El peer es el master. Solo lo devolvemos si NO es ATAPI.
    if (ch->master_is_atapi)
      return NULL;
    return (ata_device_t *)ch->master;
  }
}

// ===========================================================================
// Compatibilidad de modos master/slave.
//
// [FIX par ATA] El PIIX3 no soporta bus mastering DMA con dos discos en
// modos distintos (master en UDMA y slave en MWDMA, o viceversa). El
// controlador tiene un solo motor de bus mastering por canal y no puede
// gestionar dos timings diferentes simultáneamente. La transferencia se
// corrompe silenciosamente o se queda esperando la IRQ.
//
// La solución es la misma que aplica libata (ata_dev_pair): el modo más
// lento del par manda. Si uno de los dos solo puede MWDMA, ambos usan
// MWDMA. Si uno solo puede PIO, ambos usan PIO.
//
// Esta función se llama DESPUÉS de que ambos discos del canal hayan
// recibido su configuración preliminar (Fase A + Fase B), para que
// ata_dev_pair pueda ver el estado real de ambos.
// ===========================================================================
static int ata_apply_pair_compat(ata_device_t *dev) {
  if (!dev || !dev->channel)
    return 0;
  ata_channel_t *ch = dev->channel;

  LOG_INFO("[ATA] apply_pair_compat: %s drive=%d peer=%p master=%p slave=%p",
           dev->model, dev->drive, (void *)ata_dev_pair(dev),
           (void *)ch->master, (void *)ch->slave);

  int degraded = 0;

  // -------------------------------------------------------------------------
  // Determinar la naturaleza del peer del otro slot del canal.
  //
  // ata_dev_pair() solo devuelve peers que sean ata_device_t (ATA). Si el
  // otro slot contiene un ATAPI o está vacío, devuelve NULL. Esas dos
  // situaciones son MUY distintas y hay que distinguirlas:
  //
  //   - Peer ATAPI  → forzar PIO en el disco ATA (el CD-ROM no soporta
  //                   DMA de forma fiable en el PIIX3 y contamina al
  //                   disco si se mezclan modos).
  //   - Peer vacío  → el canal tiene un solo dispositivo. El PIIX3 SÍ
  //                   soporta DMA en este caso. Permitir el modo más
  //                   rápido que el disco soporte.
  // -------------------------------------------------------------------------
  ata_device_t *peer = ata_dev_pair(dev);
  uint8_t peer_slot_is_atapi =
      (dev->drive == 0) ? ch->slave_is_atapi : ch->master_is_atapi;

  if (!peer) {
    if (peer_slot_is_atapi) {
      // ---------------------------------------------------------------------
      // Caso 1: peer es ATAPI. Forzar PIO en el disco ATA.
      // ---------------------------------------------------------------------
      if (dev->udma_modes != 0) {
        LOG_INFO("[ATA] %s: par es ATAPI, UDMA deshabilitado", dev->model);
        dev->udma_modes = 0;
        degraded = 1;
      }
      if (dev->dma_modes != 0) {
        LOG_INFO("[ATA] %s: par es ATAPI, MWDMA deshabilitado también",
                 dev->model);
        dev->dma_modes = 0;
        degraded = 1;
      }
    } else {
      // ---------------------------------------------------------------------
      // Caso 0: canal con un solo dispositivo. El PIIX3 soporta DMA en
      // canales incompletos, así que dejamos los modos candidatos
      // intactos: ata_select_mode elegirá el más rápido disponible.
      // ---------------------------------------------------------------------
      LOG_INFO("[ATA] %s: canal sin peer, DMA permitido", dev->model);
    }
    return degraded;
  }

  // -------------------------------------------------------------------------
  // Caso 2: peer ATA. Calcular el modo común más alto que ambos soportan.
  //
  // Regla: si ambos soportan UDMA, usar el mínimo de los dos UDMA. Si
  // solo uno soporta UDMA, ambos usan MWDMA (el mínimo MWDMA común). Si
  // alguno no soporta ni UDMA ni MWDMA, ambos usan PIO.
  //
  // El objetivo es que ambos discos del canal usen el MISMO tipo de DMA.
  // Mezclar UDMA y MWDMA en el mismo canal corrompe las transferencias.
  // -------------------------------------------------------------------------

  // Determinar si ambos soportan UDMA.
  int both_udma = (dev->udma_modes != 0) && (peer->udma_modes != 0);

  if (!both_udma) {
    // No podemos usar UDMA: alguien no lo soporta. Deshabilitar UDMA en
    // ambos (el otro se encargará al procesarse).
    if (dev->udma_modes != 0) {
      LOG_INFO("[ATA] %s: peer %s sin UDMA, UDMA deshabilitado", dev->model,
               peer->model);
      dev->udma_modes = 0;
      degraded = 1;
    }
  } else {
    // Ambos soportan UDMA. Limitar al mínimo común.
    uint8_t my_max = 0;
    for (int m = 6; m >= 0; m--) {
      if (dev->udma_modes & (1 << m)) {
        my_max = (uint8_t)m;
        break;
      }
    }
    uint8_t peer_max = 0;
    for (int m = 6; m >= 0; m--) {
      if (peer->udma_modes & (1 << m)) {
        peer_max = (uint8_t)m;
        break;
      }
    }
    uint8_t common = (my_max < peer_max) ? my_max : peer_max;
    uint16_t mask = (common >= 6) ? 0x7F : ((1u << (common + 1)) - 1);
    if (dev->udma_modes & ~mask) {
      LOG_INFO("[ATA] %s: UDMA limitado a mode %u (peer %s mode %u)",
               dev->model, common, peer->model, peer_max);
      dev->udma_modes &= mask;
      degraded = 1;
    }
  }

  // -------------------------------------------------------------------------
  // Si no hay UDMA común, mirar MWDMA. Misma lógica: ambos deben
  // soportarlo, y se usa el mínimo común.
  // -------------------------------------------------------------------------
  if (dev->udma_modes == 0) {
    int both_mwdma = (dev->dma_modes != 0) && (peer->dma_modes != 0);
    if (!both_mwdma) {
      if (dev->dma_modes != 0) {
        LOG_INFO("[ATA] %s: peer %s sin MWDMA, MWDMA deshabilitado", dev->model,
                 peer->model);
        dev->dma_modes = 0;
        degraded = 1;
      }
    } else {
      uint8_t my_max = 0;
      for (int m = 2; m >= 0; m--) {
        if (dev->dma_modes & (1 << m)) {
          my_max = (uint8_t)m;
          break;
        }
      }
      uint8_t peer_max = 0;
      for (int m = 2; m >= 0; m--) {
        if (peer->dma_modes & (1 << m)) {
          peer_max = (uint8_t)m;
          break;
        }
      }
      uint8_t common = (my_max < peer_max) ? my_max : peer_max;
      uint16_t mask = (1u << (common + 1)) - 1;
      if (dev->dma_modes & ~mask) {
        LOG_INFO("[ATA] %s: MWDMA limitado a mode %u (peer %s mode %u)",
                 dev->model, common, peer->model, peer_max);
        dev->dma_modes &= mask;
        degraded = 1;
      }
    }
  }

  return degraded;
}

// ===========================================================================
// Selección de modo
// ===========================================================================
static int ata_select_mode(ata_device_t *dev) {
  // LBA48 o LBA28.
  if (dev->supports_lba48 && dev->lba48_sectors > 0) {
    dev->use_lba48 = 1;
    dev->num_sectors = dev->lba48_sectors;
  } else if (dev->supports_lba && dev->lba28_sectors > 0) {
    dev->use_lba48 = 0;
    dev->num_sectors = dev->lba28_sectors;
  } else {
    return ATA_ERR_UNKNOWN;
  }

  // PIO mode más alto.
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

  ata_set_features(dev->channel, dev->drive, ATA_FEATURE_TRANSFER_MODE,
                   pio_val);

  // [FIX] Aplicar compatibilidad de par ANTES de decidir UDMA/MWDMA.
  ata_apply_pair_compat(dev);

  // DMA: preferir UDMA > MWDMA > PIO.
  dev->use_dma = 0;
  dev->dma_mode = 0;

  int ch_idx = (dev->channel->io_base == ATA_PRIMARY_IO) ? 0 : 1;
  if (dev->supports_dma && ata_dma_is_ready(ch_idx)) {
    if (dev->udma_modes != 0) {
      for (int m = 6; m >= 0; m--) {
        if (dev->udma_modes & (1 << m)) {
          dev->dma_mode = (uint8_t)m;
          break;
        }
      }
      int rc =
          ata_set_features(dev->channel, dev->drive, ATA_FEATURE_TRANSFER_MODE,
                           0x40 | dev->dma_mode);
      if (rc == ATA_OK) {
        dev->use_dma = 1;
        LOG_INFO("[ATA] %s: usando UDMA mode %u", dev->model, dev->dma_mode);
      } else {
        LOG_WARN("[ATA] %s: SET FEATURES UDMA falló (%d), probando MWDMA",
                 dev->model, rc);
      }
    }

    if (!dev->use_dma && dev->dma_modes != 0) {
      for (int m = 2; m >= 0; m--) {
        if (dev->dma_modes & (1 << m)) {
          dev->dma_mode = (uint8_t)m;
          break;
        }
      }
      int rc =
          ata_set_features(dev->channel, dev->drive, ATA_FEATURE_TRANSFER_MODE,
                           0x20 | dev->dma_mode);
      if (rc == ATA_OK) {
        dev->use_dma = 1;
        LOG_INFO("[ATA] %s: usando MWDMA mode %u", dev->model, dev->dma_mode);
      } else {
        LOG_WARN("[ATA] %s: SET FEATURES MWDMA falló (%d)", dev->model, rc);
      }
    }
  }

  if (!dev->use_dma) {
    LOG_INFO("[ATA] %s: usando PIO mode %u", dev->model, dev->pio_mode);
  }

  LOG_INFO("[ATA] select_mode: %s use_dma=%d dma_mode=%d udma_modes=0x%x "
           "dma_modes=0x%x",
           dev->model, dev->use_dma, dev->dma_mode, dev->udma_modes,
           dev->dma_modes);

  return ATA_OK;
}

// ===========================================================================
// Emisión de comandos PIO
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
// Operaciones PIO de alto nivel
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
      // [FIX] Esperar BSY=0 tras la transferencia de escritura.
      rc = ata_wait_not_busy(dev->channel->io_base, ata_timeout_iters());
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
      // [FIX] Esperar BSY=0 tras la transferencia de escritura.
      rc = ata_wait_not_busy(dev->channel->io_base, ata_timeout_iters());
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

  // [FIX] FLUSH CACHE EXT (0xEA) solo si estamos en LBA48. En LBA28 el
  // disco solo entiende FLUSH CACHE (0xE7). Usar 0xEA en un disco LBA28
  // devuelve ABRT y no flushea nada.
  uint8_t cmd = dev->use_lba48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE;
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
// Reintentos (PIO)
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
// Politica de fallos de DMA
// ===========================================================================
#define ATA_DMA_MAX_FAILS 3

static void ata_dma_note_failure(ata_device_t *dev, int rc) {
  dev->dma_fail_streak++;
  if (rc == ATA_ERR_TIMEOUT || dev->dma_fail_streak >= ATA_DMA_MAX_FAILS) {
    dev->use_dma = 0;
    LOG_WARN("[ATA] %s: DMA desactivado (rc=%d, %u fallos consecutivos), "
             "usando PIO",
             dev->model, rc, dev->dma_fail_streak);
  } else {
    LOG_WARN("[ATA] %s: fallo de DMA (rc=%d, %u/%u), fallback a PIO solo para "
             "esta operacion",
             dev->model, rc, dev->dma_fail_streak, ATA_DMA_MAX_FAILS);
  }
}

// ===========================================================================
// block_ops_t
// ===========================================================================
static int ata_submit(block_device_t *bdev, bio_t *bio) {
  ata_device_t *dev = (ata_device_t *)bdev->private_data;
  if (!dev)
    return -ENODEV;

  ata_channel_t *ch = dev->channel;
  ata_chan_acquire(ch);
  int rc;

  switch (bio->op) {
  case BIO_READ:
    if (dev->use_dma) {
      rc = ata_dma_read(dev, bio->lba, bio->count, bio->buf);
      if (rc == ATA_OK) {
        dev->dma_fail_streak = 0;
      } else {
        ata_dma_note_failure(dev, rc);
        // [FIX] Soft reset antes de reintentar con PIO. El DMA a medias
        // puede dejar el disco en un estado inconsistente.
        ata_soft_reset(dev->channel);
        ata_select_drive(dev->channel, dev->drive);
        rc =
            ata_retry_op(dev, (void *)ata_read, bio->lba, bio->count, bio->buf);
      }
    } else {
      rc = ata_retry_op(dev, (void *)ata_read, bio->lba, bio->count, bio->buf);
    }
    break;
  case BIO_WRITE:
    if (dev->use_dma) {
      rc = ata_dma_write(dev, bio->lba, bio->count, bio->buf);
      if (rc == ATA_OK) {
        dev->dma_fail_streak = 0;
      } else {
        ata_dma_note_failure(dev, rc);
        // [FIX] Soft reset antes de reintentar con PIO.
        ata_soft_reset(dev->channel);
        ata_select_drive(dev->channel, dev->drive);
        rc = ata_retry_op(dev, (void *)ata_write, bio->lba, bio->count,
                          bio->buf);
      }
    } else {
      rc = ata_retry_op(dev, (void *)ata_write, bio->lba, bio->count, bio->buf);
    }
    break;
  case BIO_FLUSH:
    rc = ata_flush(dev);
    break;
  default:
    rc = -EINVAL;
    break;
  }

  ata_chan_release(ch);
  bio->error = (rc == ATA_OK) ? 0 : -EIO;
  return bio->error;
}

static void ata_submit_flush(block_device_t *bdev) {
  ata_device_t *dev = (ata_device_t *)bdev->private_data;
  if (!dev)
    return;
  ata_chan_acquire(dev->channel);
  (void)ata_flush(dev);
  ata_chan_release(dev->channel);
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
  LOG_INFO("    modo de transferencia: %s%s", dev->use_dma ? "DMA" : "PIO",
           dev->use_dma ? (dev->udma_modes ? " (UDMA)" : " (MWDMA)") : "");
  if (dev->use_dma) {
    LOG_INFO("    DMA mode: %u", dev->dma_mode);
  } else {
    LOG_INFO("    PIO mode: %u", dev->pio_mode);
  }
}

static block_ops_t ata_pio_ops = {
    .submit = ata_submit,
    .flush = ata_submit_flush,
    .dump = ata_submit_dump,
};

// ===========================================================================
// Registro
// ===========================================================================
static int g_next_hd_index = 0;

static void ata_name_for_index(char *out, int idx) {
  out[0] = 'h';
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
// [FIX] Fase A: detección sin registro ni selección de modo.
// ===========================================================================
static void ata_probe_device_pending(ata_channel_t *ch, uint8_t drive,
                                     const char *channel_name) {
  ata_device_t *dev = (ata_device_t *)kzalloc(sizeof(ata_device_t));
  if (!dev)
    return;
  dev->channel = ch;
  dev->drive = drive;

  int rc = ata_identify(dev);
  const char *drive_name = drive ? "slave" : "master";

  if (rc == ATA_OK) {
    dev->next = g_pending_devices;
    g_pending_devices = dev;
    g_pending_count++;

    if (drive == 0) {
      ch->master = (void *)dev;
      ch->master_is_atapi = 0; // [FIX] es ATA
    } else {
      ch->slave = (void *)dev;
      ch->slave_is_atapi = 0; // [FIX] es ATA
    }

    LOG_DEBUG("[ATA] %s %s: '%s' detectado (pendiente de finalizar)",
              channel_name, drive_name, dev->model);
  } else if (dev->is_atapi) {
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
  ch->master_is_atapi = 0; // [FIX]
  ch->slave_is_atapi = 0;  // [FIX]
  ch->busy = 0;
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

  // Inicializar los canales SIEMPRE, aunque no haya controlador. Así
  // ata_get_channel() devuelve punteros válidos y atapi_init() puede
  // comprobar ata_ide_present() antes de tocar los puertos.
  ata_init_channel(&g_channels[0], ATA_PRIMARY_IO, ATA_PRIMARY_CTRL,
                   ATA_PRIMARY_IRQ, "primario");
  ata_init_channel(&g_channels[1], ATA_SECONDARY_IO, ATA_SECONDARY_CTRL,
                   ATA_SECONDARY_IRQ, "secundario");

  pci_device_t ide;
  if (pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_STORAGE_IDE, &ide) != 0) {
    LOG_INFO("[ATA] No hay controlador IDE en PCI; driver ATA deshabilitado");
    g_ide_present = 0;
    return 0;
  }
  g_ide_present = 1;

  LOG_INFO("[ATA] Controlador IDE en %02x:%02x.%x (vendor=0x%04x "
           "device=0x%04x, progif=0x%02x)",
           ide.bus, ide.slot, ide.func, ide.vendor_id, ide.device_id,
           ide.prog_if);

  pci_enable_bus_mastering(&ide);
  pci_enable_io_mem(&ide);

  if (ata_dma_init_channel(0, ide.bus, ide.slot, ide.func) == 0) {
    extern void irq_install_handler(uint8_t irq, void (*handler)(void));
    extern void ata_dma_irq14(void);
    irq_install_handler(14, ata_dma_irq14);
    LOG_INFO("[ATA] DMA habilitado en canal primario (IRQ 14)");
  } else {
    LOG_WARN("[ATA] DMA no disponible en canal primario, usando PIO");
  }

  if (ata_dma_init_channel(1, ide.bus, ide.slot, ide.func) == 0) {
    extern void irq_install_handler(uint8_t irq, void (*handler)(void));
    extern void ata_dma_irq15(void);
    irq_install_handler(15, ata_dma_irq15);
    LOG_INFO("[ATA] DMA habilitado en canal secundario (IRQ 15)");
  } else {
    LOG_WARN("[ATA] DMA no disponible en canal secundario, usando PIO");
  }

  // Soft reset de ambos canales ANTES de habilitar nIEN=0.
  ata_soft_reset(&g_channels[0]);
  ata_soft_reset(&g_channels[1]);

  outb(g_channels[0].ctrl_base + ATA_CTRL_DEV_CTRL, 0);
  outb(g_channels[1].ctrl_base + ATA_CTRL_DEV_CTRL, 0);
  LOG_DEBUG("[ATA] IRQs del disco habilitadas (nIEN=0)");

  // Fase A: solo detección.
  ata_probe_device_pending(&g_channels[0], 0, "primario");
  ata_probe_device_pending(&g_channels[0], 1, "primario");
  ata_probe_device_pending(&g_channels[1], 0, "secundario");
  ata_probe_device_pending(&g_channels[1], 1, "secundario");

  LOG_INFO("[ATA] Slots detectados: primario master=%s slave=%s, "
           "secundario master=%s slave=%s",
           g_channels[0].master ? "sí" : "vacío",
           g_channels[0].slave ? "sí" : "vacío",
           g_channels[1].master ? "sí" : "vacío",
           g_channels[1].slave ? "sí" : "vacío");

  LOG_INFO("[ATA] Detección completada (%d discos pendientes)",
           g_pending_count);
  return 0;
}

// ===========================================================================
// [FIX] Fase C en dos pasadas:
//
//   1. Primera pasada: para cada disco, detectar LBA/PIO y guardar los
//      modos DMA candidatos (sin aplicar SET FEATURES de DMA). Aún no
//      se decide el modo final porque el peer puede no estar configurado.
//
//   2. Segunda pasada: aplicar ata_apply_pair_compat (que ya ve el
//      estado de ambos discos), aplicar SET FEATURES del modo final, y
//      registrar en el block layer.
// ===========================================================================
int ata_pio_finalize(void) {
  int registered = 0;

  // --- Primera pasada: detectar LBA/PIO y guardar candidatos DMA ---
  for (ata_device_t *dev = g_pending_devices; dev; dev = dev->next) {
    // LBA48 o LBA28.
    if (dev->supports_lba48 && dev->lba48_sectors > 0) {
      dev->use_lba48 = 1;
      dev->num_sectors = dev->lba48_sectors;
    } else if (dev->supports_lba && dev->lba28_sectors > 0) {
      dev->use_lba48 = 0;
      dev->num_sectors = dev->lba28_sectors;
    } else {
      LOG_WARN("[ATA] %s: sin LBA soportado, saltando", dev->model);
      continue;
    }

    // PIO mode más alto.
    dev->pio_mode = 0;
    if (dev->pio_modes & (1 << 4))
      dev->pio_mode = 4;
    else if (dev->pio_modes & (1 << 3))
      dev->pio_mode = 3;
    else if (dev->pio_modes & (1 << 2))
      dev->pio_mode = 2;
    else if (dev->pio_modes & (1 << 1))
      dev->pio_mode = 1;

    // [FIX] NO aplicar SET FEATURES de PIO ni DMA todavía. Solo guardar
    // los candidatos. La decisión final se toma en la segunda pasada.
    dev->use_dma = 0;
    dev->dma_mode = 0;
  }

  // --- Segunda pasada: aplicar compatibilidad de par y SET FEATURES ---
  for (ata_device_t *dev = g_pending_devices; dev; dev = dev->next) {
    if (dev->num_sectors == 0)
      continue;

    // PIO: SET FEATURES del modo PIO (siempre, es el fallback).
    uint8_t pio_val = dev->pio_mode;
    if (dev->pio_mode >= 3)
      pio_val = dev->pio_mode - 3 + 0x08;
    ata_set_features(dev->channel, dev->drive, ATA_FEATURE_TRANSFER_MODE,
                     pio_val);

    // Aplicar compatibilidad de par. Ahora ambos discos del canal ya
    // tienen sus udma_modes/dma_modes calculados.
    ata_apply_pair_compat(dev);

    // Intentar activar UDMA si queda alguno disponible.
    int ch_idx = (dev->channel->io_base == ATA_PRIMARY_IO) ? 0 : 1;
    if (dev->supports_dma && ata_dma_is_ready(ch_idx)) {
      if (dev->udma_modes != 0) {
        for (int m = 6; m >= 0; m--) {
          if (dev->udma_modes & (1 << m)) {
            dev->dma_mode = (uint8_t)m;
            break;
          }
        }
        int rc =
            ata_set_features(dev->channel, dev->drive,
                             ATA_FEATURE_TRANSFER_MODE, 0x40 | dev->dma_mode);
        if (rc == ATA_OK) {
          dev->use_dma = 1;
          LOG_INFO("[ATA] %s: usando UDMA mode %u", dev->model, dev->dma_mode);
        } else {
          LOG_WARN("[ATA] %s: SET FEATURES UDMA falló (%d)", dev->model, rc);
        }
      }

      if (!dev->use_dma && dev->dma_modes != 0) {
        for (int m = 2; m >= 0; m--) {
          if (dev->dma_modes & (1 << m)) {
            dev->dma_mode = (uint8_t)m;
            break;
          }
        }
        int rc =
            ata_set_features(dev->channel, dev->drive,
                             ATA_FEATURE_TRANSFER_MODE, 0x20 | dev->dma_mode);
        if (rc == ATA_OK) {
          dev->use_dma = 1;
          LOG_INFO("[ATA] %s: usando MWDMA mode %u", dev->model, dev->dma_mode);
        } else {
          LOG_WARN("[ATA] %s: SET FEATURES MWDMA falló (%d)", dev->model, rc);
        }
      }
    }

    if (!dev->use_dma) {
      LOG_INFO("[ATA] %s: usando PIO mode %u", dev->model, dev->pio_mode);
    }

    // Registrar en el block layer.
    char name[8];
    ata_name_for_index(name, g_next_hd_index);
    LOG_INFO(
        "[ATA] %s en %s %s: '%s' (%lu sectores, %lu MB, %s, %s)", name,
        dev->channel->name, dev->drive ? "slave" : "master", dev->model,
        (unsigned long)dev->num_sectors,
        (unsigned long)(dev->num_sectors * dev->sector_size / (1024 * 1024)),
        dev->use_lba48 ? "LBA48" : "LBA28", dev->use_dma ? "DMA" : "PIO");
    if (ata_register_device(dev, name) != 0) {
      LOG_WARN("[ATA] %s: fallo al registrar %s", dev->model, name);
      continue;
    }
    g_next_hd_index++;
    registered++;
  }

  // Liberar la lista temporal.
  ata_device_t *dev = g_pending_devices;
  while (dev) {
    ata_device_t *next = dev->next;
    dev->next = g_devices;
    g_devices = dev;
    dev = next;
  }
  g_pending_devices = NULL;
  g_device_count = registered;

  LOG_INFO("[ATA] %d discos registrados", registered);
  return registered;
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