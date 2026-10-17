/* bootloader/bios/int13.c */
#include "bios.h"

extern int rm_int13_read(uint8_t drive, uint32_t lba, uint16_t count,
                         void *buf);

/* Lee `count` sectores de `sector_bytes` cada uno desde `lba` a `buf`.
 * Si `buf` está por debajo de 1 MB, lectura directa. Si no, bounce. */
static int int13_read_impl(uint8_t drive, uint32_t lba, uint16_t count,
                           uint32_t sector_bytes, void *buf) {
  if (count == 0)
    return 0;

  uint32_t addr = (uint32_t)buf;
  uint32_t total_bytes = (uint32_t)count * sector_bytes;

  /* [FIX] Usar el buffer directamente SOLO si está en la zona segura
   * (0x1000 a 0x90000). Si está en la pila o más arriba, usamos bounce
   * porque las BIOS antiguas fallan al escribir tan alto. */
  if (addr >= 0x1000u && (addr + total_bytes) <= 0x90000u) {
    return rm_int13_read(drive, lba, count, buf) == 0 ? 0 : -1;
  }

  /* Buffer alto o en la pila: bounce por chunks. */
  uint8_t *dst = (uint8_t *)buf;
  uint32_t remaining = total_bytes;
  uint32_t cur_lba = lba;

  while (remaining) {
    uint16_t chunk_sec =
        (uint16_t)((remaining + sector_bytes - 1) / sector_bytes);
    uint32_t max_sec = BOUNCE_BUF_SIZE / sector_bytes; if (chunk_sec > max_sec)
      chunk_sec = (uint16_t)max_sec;
    uint32_t chunk_bytes = (uint32_t)chunk_sec * sector_bytes;
    if (chunk_bytes > remaining)
      chunk_bytes = remaining;

    if (rm_int13_read(drive, cur_lba, chunk_sec, (void *)BOUNCE_BUF_ADDR) != 0)
      return -1;
    copy_mem(dst, (void *)BOUNCE_BUF_ADDR, chunk_bytes);

    dst += chunk_bytes;
    remaining -= chunk_bytes;
    cur_lba += chunk_sec;
  }
  return 0;
}

int int13_read(uint8_t drive, uint32_t lba, uint16_t count, void *buf) {
  return int13_read_impl(drive, lba, count, 2048, buf);
}

int int13_read_512(uint8_t drive, uint32_t lba, uint16_t count, void *buf) {
  return int13_read_impl(drive, lba, count, 512, buf);
}

int int13_reset(uint8_t drive) {
  (void)drive;
  return 0;
}

int int13_get_params(uint8_t drive, uint16_t *sector_size_out,
                     uint32_t *total_sectors_out) {
  (void)drive;
  if (sector_size_out)
    *sector_size_out = 2048;
  if (total_sectors_out)
    *total_sectors_out = 0;
  return 0;
}