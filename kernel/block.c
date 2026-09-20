// kernel/block.c
//
// Implementación de la capa de bloques. Ver block.h para el contrato.

#include "block.h"
#include "klog.h"
#include "string.h"

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
static block_device_t *g_devices = NULL;
static int g_device_count = 0;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void block_init(void) {
  g_devices = NULL;
  g_device_count = 0;
  LOG_INFO("[BLOCK] Subsistema de bloques inicializado");
}

// ---------------------------------------------------------------------------
// Registro
// ---------------------------------------------------------------------------
int block_register(block_device_t *dev) {
  if (!dev) {
    LOG_ERR("[BLOCK] register: dev == NULL");
    return -1;
  }
  if (dev->name[0] == '\0') {
    LOG_ERR("[BLOCK] register: nombre vacío");
    return -1;
  }
  if (dev->sector_size == 0) {
    LOG_ERR("[BLOCK] register(%s): sector_size == 0", dev->name);
    return -1;
  }
  if (dev->num_sectors == 0) {
    LOG_ERR("[BLOCK] register(%s): num_sectors == 0", dev->name);
    return -1;
  }
  if (!dev->read) {
    LOG_ERR("[BLOCK] register(%s): read == NULL", dev->name);
    return -1;
  }
  if (!dev->is_read_only && !dev->write) {
    LOG_ERR("[BLOCK] register(%s): write == NULL pero no es read-only",
            dev->name);
    return -1;
  }
  if (g_device_count >= BLOCK_MAX_DEVICES) {
    LOG_ERR("[BLOCK] register(%s): tabla llena (%d/%d)", dev->name,
            g_device_count, BLOCK_MAX_DEVICES);
    return -1;
  }

  // Comprobar nombre duplicado.
  for (block_device_t *p = g_devices; p; p = p->next) {
    if (strcmp(p->name, dev->name) == 0) {
      LOG_ERR("[BLOCK] register: nombre duplicado '%s'", dev->name);
      return -1;
    }
  }

  // Insertar al principio de la lista.
  dev->next = g_devices;
  g_devices = dev;
  g_device_count++;

  // Log informativo. El tamaño se calcula en MB.
  uint64_t size_bytes = dev->num_sectors * (uint64_t)dev->sector_size;
  uint64_t size_mb = size_bytes / (1024 * 1024);
  LOG_INFO("[BLOCK] Registrado %s: %lu sectores de %u bytes (%lu MB)%s",
           dev->name, (unsigned long)dev->num_sectors, dev->sector_size,
           (unsigned long)size_mb, dev->is_read_only ? " [RO]" : "");

  return 0;
}

// ---------------------------------------------------------------------------
// Búsqueda
// ---------------------------------------------------------------------------
block_device_t *block_get(const char *name) {
  if (!name)
    return NULL;
  for (block_device_t *p = g_devices; p; p = p->next) {
    if (strcmp(p->name, name) == 0)
      return p;
  }
  return NULL;
}

block_device_t *block_get_by_index(int index) {
  if (index < 0)
    return NULL;
  int i = 0;
  for (block_device_t *p = g_devices; p; p = p->next) {
    if (i == index)
      return p;
    i++;
  }
  return NULL;
}

int block_count(void) { return g_device_count; }

// ---------------------------------------------------------------------------
// Helpers de validación
// ---------------------------------------------------------------------------
static int validate_lba(block_device_t *dev, uint64_t lba, uint32_t count) {
  // count == 0 no tiene sentido.
  if (count == 0)
    return BLOCK_EINVAL;
  // Overflow en lba + count.
  if (lba > dev->num_sectors)
    return BLOCK_ERANGE;
  if (count > dev->num_sectors - lba)
    return BLOCK_ERANGE;
  return BLOCK_OK;
}

// ---------------------------------------------------------------------------
// Read / Write / Flush
// ---------------------------------------------------------------------------
int block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf) {
  if (!dev || !buf)
    return BLOCK_EINVAL;

  int rc = validate_lba(dev, lba, count);
  if (rc != BLOCK_OK)
    return rc;

  return dev->read(dev, lba, count, buf);
}

int block_write(block_device_t *dev, uint64_t lba, uint32_t count,
                const void *buf) {
  if (!dev || !buf)
    return BLOCK_EINVAL;

  if (dev->is_read_only)
    return BLOCK_EROFS;

  int rc = validate_lba(dev, lba, count);
  if (rc != BLOCK_OK)
    return rc;

  if (!dev->write)
    return BLOCK_EIO;

  return dev->write(dev, lba, count, buf);
}

int block_flush(block_device_t *dev) {
  if (!dev)
    return BLOCK_EINVAL;
  if (dev->is_read_only)
    return BLOCK_OK; // Nada que flushear.
  if (!dev->flush)
    return BLOCK_OK; // El driver no lo necesita.
  return dev->flush(dev);
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
void block_dump(void) {
  LOG_INFO("[BLOCK] Discos registrados (%d):", g_device_count);
  for (block_device_t *p = g_devices; p; p = p->next) {
    uint64_t size_bytes = p->num_sectors * (uint64_t)p->sector_size;
    uint64_t size_mb = size_bytes / (1024 * 1024);
    LOG_INFO("  %s: %lu sectores, %u bytes/sector, %lu MB%s", p->name,
             (unsigned long)p->num_sectors, p->sector_size,
             (unsigned long)size_mb, p->is_read_only ? " [RO]" : "");
  }
}