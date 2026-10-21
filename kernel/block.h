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
//   Driver (ata_pio, ahci, ...) ← habla con el hardware
//
// CONTRATO ASYNC (Fase 1.4)
// -------------------------
//
// El block layer soporta drivers SÍNCRONOS y ASÍNCRONOS.
//
//   SÍNCRONO:
//     - ops->submit hace todo el trabajo antes de retornar.
//     - Rellena bio->error.
//     - Devuelve 0 (OK) o <0 (error). NUNCA -EINPROGRESS.
//     - El block layer NO llama a bio_endio (el driver ya terminó).
//
//   ASÍNCRONO:
//     - ops->submit arranca el trabajo y devuelve -EINPROGRESS.
//     - bio->error se rellena MÁS TARDE.
//     - El driver llama a bio_endio(bio, error) cuando termina, desde
//       cualquier contexto (IRQ, kthread, workqueue).
//     - El llamante NO debe tocar el bio hasta que se invoque end_io.
//
// REGLA CRÍTICA: si ops->submit devuelve -EINPROGRESS, el llamante DEBE
// haber puesto bio->end_io != NULL. Si no, el bio se pierde (el driver
// llamará a bio_endio con end_io NULL y no pasará nada).
//
// Los wrappers bdev_read/bdev_write/bdev_flush SIEMPRE son síncronos
// desde el punto de vista del llamante: internamente usan completion_t
// y esperan. Esto permite que el llamante no tenga que saber si el
// driver subyacente es sync o async.
//
// Para I/O verdaderamente asíncrona (kthread que solapa I/O con CPU),
// usar blk_submit directamente con un end_io propio (p.ej. completion_t
// embebida en el contexto del llamante).
// ---------------------------------------------------------------------------

enum bio_op {
  BIO_READ = 0,
  BIO_WRITE = 1,
  BIO_FLUSH = 2,
};

struct bio;

typedef void (*bio_end_io_fn)(struct bio *bio);

typedef struct bio {
  struct block_device *bdev;
  uint64_t lba;
  uint32_t count;
  void *buf;
  int op;
  int error;
  bio_end_io_fn end_io;
  void *end_io_data;
  struct bio *next; // reservado para encadenamiento futuro
} bio_t;

struct block_ops;

typedef struct block_device {
  char name[16];
  uint64_t num_sectors;
  uint32_t sector_size;
  int is_read_only;
  int is_partition;
  struct block_device *parent;
  uint64_t start_lba;

  struct block_ops *ops;
  void *private_data;

  struct block_device *next;
} block_device_t;

typedef struct block_ops {
  int (*submit)(block_device_t *bdev, bio_t *bio);
  void (*flush)(block_device_t *bdev);
  void (*dump)(block_device_t *bdev);
} block_ops_t;

// ---------------------------------------------------------------------------
// Core API
// ---------------------------------------------------------------------------
void blk_init(void);
int blk_register(block_device_t *bdev);
int blk_unregister(block_device_t *bdev);
block_device_t *blk_lookup(const char *name);
block_device_t *blk_get_by_index(int index);
int blk_count(void);

// Envía un bio. Ver CONTRATO ASYNC arriba.
//   Devuelve 0             → terminado OK (driver síncrono).
//   Devuelve <0, != -EINPROGRESS → error (driver síncrono).
//   Devuelve -EINPROGRESS  → en vuelo; driver llamará bio_endio.
int blk_submit(bio_t *bio);

// Marca el bio como completado y llama a end_io. IRQ-safe.
void bio_endio(bio_t *bio, int error);

// Envía un bio con callback de finalización. Azúcar sobre blk_submit:
//   - rellena bio->end_io / bio->end_io_data
//   - llama blk_submit
//   - devuelve lo que devuelva blk_submit
// Si el driver es síncrono, blk_submit devuelve 0 y bio_endio YA se
// llamó (por el propio driver). Si es async, devuelve -EINPROGRESS y
// bio_endio se llamará más tarde.
//
// IMPORTANTE: si blk_submit devuelve 0, el bio ya terminó. Si devuelve
// -EINPROGRESS, el bio sigue vivo y NO debes tocarlo hasta que cb
// corra. Si devuelve otro negativo, el bio no llegó a arrancar (o
// falló al arrancar) y NO se llamará a cb.
int blk_submit_async(bio_t *bio, bio_end_io_fn cb, void *ctx);

// Envía un bio y espera a que termine (sync wrapper). Construye una
// completion_t en el stack del llamante, la instala como end_io_data,
// y llama blk_submit. Si blk_submit devuelve -EINPROGRESS, espera.
//
// Devuelve bio->error, o el rc de blk_submit si éste falló sin poner
// el bio en vuelo.
int blk_submit_wait(bio_t *bio);

void blk_dump(void);

// ---------------------------------------------------------------------------
// Wrappers síncronos (bdev_*). Siempre bloquean al llamante.
// ---------------------------------------------------------------------------
int bdev_read(block_device_t *bdev, uint64_t lba, uint32_t count, void *buf);
int bdev_write(block_device_t *bdev, uint64_t lba, uint32_t count,
               const void *buf);
int bdev_flush(block_device_t *bdev);

static inline uint64_t bdev_size_bytes(const block_device_t *bdev) {
  return bdev->num_sectors * (uint64_t)bdev->sector_size;
}

static inline uint64_t bdev_size_mb(const block_device_t *bdev) {
  return bdev_size_bytes(bdev) / (1024 * 1024);
}

#endif