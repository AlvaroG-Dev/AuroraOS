// kernel/block.h
#ifndef KERNEL_BLOCK_H
#define KERNEL_BLOCK_H

#include "uaccess.h"
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Block layer.
//
// Arquitectura por capas:
//
//   FS (FAT32, ext2, ...)
//     ↓  bdev_read / bdev_write
//   Block I/O (bio)      ← una operación de lectura/escritura
//     ↓  blk_submit
//   Block core           ← registro de discos, dispatch
//     ↓  ops->submit
//   Driver (ata_pio, ...) ← habla con el hardware
//
// El driver NO sabe de FS ni de caché. Solo ejecuta bios.
// El FS NO sabe de hardware. Solo construye bios.
//
// NOMENCLATURA:
//   bio_*    → operaciones sobre un bio
//   blk_*    → core del block layer (registro, dispatch)
//   bdev_*   → operaciones sobre un block_device (read/write)
//
// ERRORES: se usan los de uaccess.h (EINVAL, ERANGE, EIO, EROFS, ...).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// bio: una operación de I/O.
//
// El FS construye un bio, lo envía al block layer, y espera a que termine.
// El driver lo ejecuta y rellena `error`. Si es asíncrono, llama a
// bio_endio() cuando termina.
// ---------------------------------------------------------------------------

enum bio_op {
  BIO_READ = 0,
  BIO_WRITE = 1,
  BIO_FLUSH = 2,
};

struct bio;

// Callback opcional. El driver lo llama (a través de bio_endio) cuando
// el bio termina. Puede ser NULL.
typedef void (*bio_end_io_fn)(struct bio *bio);

typedef struct bio {
  struct block_device *bdev; // disco destino
  uint64_t lba;         // sector inicial (en unidades de bdev->sector_size)
  uint32_t count;       // número de sectores
  void *buf;            // buffer del kernel (no userland)
  int op;               // enum bio_op
  int error;            // 0 = OK, negativo = error
  bio_end_io_fn end_io; // callback al terminar (o NULL)
  void *end_io_data;    // datos para el callback
  struct bio *next;     // para encadenar bios (futuro)
} bio_t;

// ---------------------------------------------------------------------------
// block_device: una instancia montable.
//
// Puede ser un disco físico completo (hda) o una partición (hda1).
// El driver lo rellena y lo registra con blk_register().
// ---------------------------------------------------------------------------

struct block_ops;

typedef struct block_device {
  char name[16];               // "hda", "hda1", "sda", "sr0", ...
  uint64_t num_sectors;        // tamaño total en sectores
  uint32_t sector_size;        // 512 (disco) o 2048 (CD/DVD)
  int is_read_only;            // 1 para CDs/DVDs
  int is_partition;            // 0 = disco físico, 1 = partición
  struct block_device *parent; // si es partición, apunta al disco
  uint64_t start_lba;          // si es partición, offset en el disco

  struct block_ops *ops; // operaciones del driver
  void *private_data;    // datos privados del driver

  struct block_device *next; // lista enlazada interna
} block_device_t;

// ---------------------------------------------------------------------------
// Contrato de bio->end_io y ops->submit.
//
// Un driver puede ser:
//
//   SÍNCRONO:
//     - Hace el trabajo completo dentro de submit().
//     - Rellena bio->error.
//     - Devuelve 0 (OK) o <0 (error, nunca -EINPROGRESS).
//     - El block layer llama a bio_endio() al final (si end_io != NULL).
//
//   ASÍNCRONO:
//     - Arranca el trabajo y devuelve -EINPROGRESS.
//     - Rellena bio->error más tarde.
//     - Llama a bio_endio(bio, error) cuando termina, desde cualquier
//       contexto (IRQ handler, workqueue, ...).
//     - El llamante NO debe tocar el bio hasta que se llame end_io.
//
// El llamante que use blk_submit directamente debe:
//   - Poner end_io si espera -EINPROGRESS.
//   - Tratar cualquier otro retorno como "ya terminado".
//
// Los wrappers bdev_read/bdev_write/bdev_flush son SIEMPRE síncronos:
// internamente usan completion para esperar al end_io.
// ---------------------------------------------------------------------------

typedef struct block_ops {
  int (*submit)(block_device_t *bdev, bio_t *bio);
  void (*flush)(block_device_t *bdev);
  void (*dump)(block_device_t *bdev);
} block_ops_t;

// ---------------------------------------------------------------------------
// API del core (blk_*)
// ---------------------------------------------------------------------------

// Inicializa el block layer. Llamar UNA VEZ en el boot, antes de que
// los drivers registren discos.
void blk_init(void);

// Registra un block_device. Valida:
//   - name no vacío y único
//   - sector_size > 0
//   - num_sectors > 0
//   - ops != NULL y ops->submit != NULL
// Devuelve 0 si OK, negativo si error.
int blk_register(block_device_t *bdev);

// Desregistra un block_device. Busca por puntero.
// Devuelve 0 si OK, -ENOENT si no estaba registrado.
int blk_unregister(block_device_t *bdev);

// Busca un disco por nombre. NULL si no existe.
block_device_t *blk_lookup(const char *name);

// I-ésimo disco (0-based). NULL si i >= blk_count().
block_device_t *blk_get_by_index(int index);

// Número de discos registrados.
int blk_count(void);

// Envía un bio al block layer.
//
// Devuelve:
//   0             el bio terminó bien (driver síncrono).
//   <0, != -EINPROGRESS   error al enviar o al ejecutar.
//   -EINPROGRESS  el bio está en vuelo; el driver llamará a end_io.
//
// Si devuelve -EINPROGRESS y bio->end_io es NULL, el comportamiento
// es indefinido (el bio se pierde).
int blk_submit(bio_t *bio);

// Marca un bio como completado y llama a su end_io (si existe).
// Los drivers asíncronos llaman a esta función al terminar.
// Es seguro llamarla desde IRQ handler.
void bio_endio(bio_t *bio, int error);

// Debug: imprime la lista de discos registrados.
void blk_dump(void);

// ---------------------------------------------------------------------------
// API del dispositivo (bdev_*)
//
// Son wrappers síncronos sobre blk_submit. Construyen un bio, lo envían,
// y esperan a que termine (asumiendo drivers síncronos por ahora).
//
// En Fase 1 (PIO), todos los drivers son síncronos. En Fase 3 (DMA),
// estos wrappers pasarán a usar completion.
// ---------------------------------------------------------------------------

int bdev_read(block_device_t *bdev, uint64_t lba, uint32_t count, void *buf);
int bdev_write(block_device_t *bdev, uint64_t lba, uint32_t count,
               const void *buf);
int bdev_flush(block_device_t *bdev);

// Helpers
static inline uint64_t bdev_size_bytes(const block_device_t *bdev) {
  return bdev->num_sectors * (uint64_t)bdev->sector_size;
}

static inline uint64_t bdev_size_mb(const block_device_t *bdev) {
  return bdev_size_bytes(bdev) / (1024 * 1024);
}

#endif