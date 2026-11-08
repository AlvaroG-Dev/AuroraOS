// kernel/bcache.h
//
// Caché de bloques genérica (para ext2 y futuros FS con bloques de 1-4 KiB).
// Independiente del FS: cachea (bdev, nº de bloque lógico) con un tamaño de
// bloque `bs` que elige el llamante (potencia de 2, >= sector_size del
// dispositivo, <= 4096). Un mismo bdev debe usar siempre el mismo `bs`.
//
// Modelo: write-back. bc_get() devuelve el búfer con una referencia; el
// llamante modifica bc_data() y llama a bc_mark_dirty() antes de bc_put().
// Los búfers sucios se escriben en bc_sync_dev()/bc_sync_all() o al ser
// expulsados (LRU, solo los de refs == 0). Un hilo de write-back periódico
// se añadirá con ext2 (E2); aquí solo está el mecanismo.
//
// Concurrencia v1: un único mutex global que se mantiene durante la E/S
// (simple y correcto; se afinará en E7).
#ifndef KERNEL_BCACHE_H
#define KERNEL_BCACHE_H

#include "block.h"
#include <stddef.h>
#include <stdint.h>

typedef struct bc_buf bc_buf_t;

void bc_init(void);

// Lee (si no está en caché) el bloque `blk` y devuelve el búfer con una
// referencia. NULL si falla la E/S o los argumentos no son válidos.
bc_buf_t *bc_get(block_device_t *bdev, uint64_t blk, uint32_t bs);
// Como bc_get pero sin leer del disco: el contenido sale a cero. Para
// bloques recién asignados que se van a sobrescribir entero.
bc_buf_t *bc_get_new(block_device_t *bdev, uint64_t blk, uint32_t bs);

// El búfer si ya está en caché (con referencia), NULL si no (no lee del disco).
bc_buf_t *bc_get_cached(block_device_t *bdev, uint64_t blk, uint32_t bs);
// 1 si el bloque ya está en caché (sin leerlo).
int bc_present(block_device_t *bdev, uint64_t blk);
// Lee de una sola petición hasta `count` bloques consecutivos que falten (se
// para en el primero presente). Devuelve cuántos cargó. Sin referencias.
int bc_readahead(block_device_t *bdev, uint64_t blk, uint32_t count,
                 uint32_t bs);

void *bc_data(bc_buf_t *b);
uint32_t bc_size(const bc_buf_t *b);
uint64_t bc_block_no(const bc_buf_t *b);
void bc_mark_dirty(bc_buf_t *b);
// Datos de ficheros: se vuelcan antes que los metadatos (data=ordered).
void bc_mark_dirty_data(bc_buf_t *b);
void bc_put(bc_buf_t *b);

// Escribe todos los búfers sucios de `bdev` y hace bdev_flush. 0 o -EIO.
int bc_sync_dev(block_device_t *bdev);
int bc_sync_all(void);
// Descarta todos los búfers de `bdev` (tras sync). -EBUSY si alguno tiene
// referencias. Se usa en umount para que el siguiente montaje relea el disco.
int bc_invalidate_dev(block_device_t *bdev);

// Journal (jbd2) de un dispositivo: los metadatos sucios (los que no son datos de
// ficheros) se escriben como UNA transacción por commit() y solo después en su
// sitio; luego done() deja el journal vacío. commit() se llama con la caché
// bloqueada: no puede llamar a bc_*. max_blocks = bloques por transacción.
typedef int (*bc_commit_fn)(void *priv, bc_buf_t **bufs, uint32_t n);
typedef void (*bc_done_fn)(void *priv);
void bc_set_journal(block_device_t *bdev, bc_commit_fn commit, bc_done_fn done,
                    void *priv, uint32_t max_blocks); // commit == NULL: quitarlo

// Cota de memoria en bytes (por defecto 16 MiB). Expulsa lo que sobre.
void bc_set_limit(size_t bytes);
// Cota según la RAM (1/4, 16..512 MiB) y reclamo bajo presión de memoria.
void bc_autosize(void);
size_t bc_reclaim(size_t bytes);

struct bc_stats {
  uint64_t hits, misses, evictions, writes, bufs, dirty;
};
void bc_get_stats(struct bc_stats *out);

#endif
