// kernel/block.h
#ifndef KERNEL_BLOCK_H
#define KERNEL_BLOCK_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Capa de bloques (block layer).
//
// Es la abstracción entre los drivers de disco (ATA, ATAPI, AHCI, USB,
// NVMe, ...) y los sistemas de archivos (FAT32, ext2, ...).
//
// Un "disco" (block_device_t) expone lecturas y escrituras de sectores.
// El FS no sabe si el disco es ATA, SATA, USB o NVMe: solo llama a
// block_read/block_write y recibe sectores.
//
// CONTRATO:
//
//   - Los drivers de disco registran sus dispositivos con
//     block_register(). El nombre es único ("hda", "sda", ...).
//
//   - Los FS usan block_read/block_write. Estas funciones validan los
//     parámetros (LBA dentro de rango, count > 0, buf != NULL) y llaman
//     al método correspondiente del driver.
//
//   - read/write son SÍNCRONOS: bloquean hasta que el disco termina.
//
//   - El buffer (buf) debe ser del kernel. Si viene de userland, el
//     llamante debe copiarlo antes (copy_from_user / copy_to_user).
//
//   - sector_size típicamente es 512 (discos) o 2048 (CD/DVD).
// ---------------------------------------------------------------------------

#define BLOCK_MAX_DEVICES 16
#define BLOCK_NAME_MAX 16

struct block_device;

// Métodos del driver. Devuelven 0 si OK, negativo si error.
typedef int (*block_read_fn)(struct block_device *dev, uint64_t lba,
                             uint32_t count, void *buf);
typedef int (*block_write_fn)(struct block_device *dev, uint64_t lba,
                              uint32_t count, const void *buf);
typedef int (*block_flush_fn)(struct block_device *dev);

typedef struct block_device {
  char name[BLOCK_NAME_MAX]; // "hda", "sda", "sr0", ...
  uint32_t sector_size;      // 512 o 2048
  uint64_t num_sectors;      // tamaño total en sectores
  int is_read_only;          // 1 para CDs/DVDs, 0 para discos normales

  block_read_fn read;
  block_write_fn write;      // NULL si is_read_only
  block_flush_fn flush;      // NULL si no soporta flush

  void *priv;                // datos del driver (ata_device_t*, ...)
  struct block_device *next; // lista enlazada interna
} block_device_t;

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

// Inicializa el subsistema. Llamar UNA VEZ durante el boot, antes de
// que los drivers registren discos.
void block_init(void);

// Registra un disco. Devuelve 0 si OK, -1 si:
//   - el nombre está duplicado
//   - no hay hueco en la tabla
//   - los parámetros son inválidos
int block_register(block_device_t *dev);

// Busca un disco por nombre. Devuelve NULL si no existe.
block_device_t *block_get(const char *name);

// Devuelve el i-ésimo disco (0-based). NULL si i >= número de discos.
block_device_t *block_get_by_index(int index);

// Número de discos registrados.
int block_count(void);

// Lectura/escritura. Valida los parámetros y llama al driver.
// Devuelven 0 si OK, negativo si error.
//
// Errores:
//   -EINVAL   parámetros inválidos (buf NULL, count 0, ...)
//   -ERANGE   lba + count fuera de rango
//   -EROFS    disco de solo lectura (write)
//   -EIO      error del driver
int block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf);
int block_write(block_device_t *dev, uint64_t lba, uint32_t count,
                const void *buf);
int block_flush(block_device_t *dev);

// Debug: imprime la lista de discos.
void block_dump(void);

// Errores (negativos). Definidos aquí para no depender de uaccess.h.
#define BLOCK_OK 0
#define BLOCK_EINVAL (-22)
#define BLOCK_ERANGE (-34)
#define BLOCK_EROFS (-30)
#define BLOCK_EIO (-5)

#endif