// kernel/block.c
//
// Implementación del block layer. Ver block.h para el contrato.

#include "block.h"
#include "completion.h"
#include "klog.h"
#include "string.h"
#include "uaccess.h" // EINVAL, ERANGE, EIO, EROFS, ENOENT

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
static block_device_t *g_devices = NULL;
static int g_device_count = 0;

// Callback que se ejecuta cuando el driver termina el bio.
// Marca la completion asociada.
static void bdev_completion_cb(bio_t *bio) {
  completion_t *comp = (completion_t *)bio->end_io_data;
  if (comp)
    complete(comp);
}

// Helper común para bdev_read/bdev_write.
// Devuelve 0 si OK, <0 si error. Asume validate_io ya hecho.
// Helper común para bdev_read/bdev_write.
// Devuelve 0 si OK, <0 si error. Asume validate_io ya hecho.
static int bdev_submit_sync(bio_t *bio) {
  // [DEBUG RBP] Capturar RBP al entrar.
  uint64_t rbp_in;
  __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_in));

  completion_t comp;
  completion_init(&comp);

  bio->end_io = bdev_completion_cb;
  bio->end_io_data = &comp;
  bio->error = 0;

  int rc = blk_submit(bio);

  // [DEBUG RBP] Verificar que RBP no cambió tras blk_submit.
  uint64_t rbp_post_submit;
  __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_post_submit));
  if (rbp_in != rbp_post_submit) {
    LOG_ERR("[BSYNC] RBP cambió en blk_submit: %p -> %p", (void *)rbp_in,
            (void *)rbp_post_submit);
    __asm__ volatile("mov %0, %%rbp" : : "r"(rbp_in));
  }

  // Si el driver es síncrono, rc == 0 o <0 (nunca -EINPROGRESS) y
  // bio_endio ya se llamó (o no hacía falta).
  if (rc == -EINPROGRESS) {
    // [DEBUG RBP] Capturar antes de esperar.
    uint64_t rbp_before_wait;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_before_wait));

    wait_for_completion(&comp);

    // [DEBUG RBP] Capturar después de esperar.
    uint64_t rbp_after_wait;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_after_wait));
    if (rbp_before_wait != rbp_after_wait) {
      LOG_ERR("[BSYNC] RBP cambió en wait_for_completion: %p -> %p",
              (void *)rbp_before_wait, (void *)rbp_after_wait);
      // Restaurar para poder seguir.
      __asm__ volatile("mov %0, %%rbp" : : "r"(rbp_before_wait));
    }
    return bio->error;
  }

  // Síncrono: el driver ya rellenó bio->error.
  return (rc < 0) ? rc : bio->error;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void blk_init(void) {
  g_devices = NULL;
  g_device_count = 0;
  LOG_INFO("[BLK] Block layer inicializado");
}

// ---------------------------------------------------------------------------
// Registro
// ---------------------------------------------------------------------------
static int validate_device(block_device_t *bdev) {
  if (!bdev)
    return -EINVAL;
  if (bdev->name[0] == '\0')
    return -EINVAL;
  if (bdev->sector_size == 0)
    return -EINVAL;
  if (bdev->num_sectors == 0)
    return -EINVAL;
  if (!bdev->ops || !bdev->ops->submit)
    return -EINVAL;
  if (!bdev->is_read_only && !bdev->ops->submit) // redundante, pero explícito
    return -EINVAL;
  return 0;
}

static int name_exists(const char *name) {
  for (block_device_t *p = g_devices; p; p = p->next) {
    if (strcmp(p->name, name) == 0)
      return 1;
  }
  return 0;
}

int blk_register(block_device_t *bdev) {
  int rc = validate_device(bdev);
  if (rc < 0) {
    LOG_ERR("[BLK] register: dispositivo inválido (rc=%d)", rc);
    return rc;
  }
  if (name_exists(bdev->name)) {
    LOG_ERR("[BLK] register: nombre duplicado '%s'", bdev->name);
    return -EINVAL;
  }
  if (g_device_count >= 16) {
    LOG_ERR("[BLK] register(%s): tabla llena", bdev->name);
    return -ENOMEM;
  }

  // Insertar AL FINAL: blk_get_by_index(0) es el primer dispositivo
  // registrado (orden de deteccion), y blk_dump lista en ese mismo orden.
  bdev->next = NULL;
  if (!g_devices) {
    g_devices = bdev;
  } else {
    block_device_t *tail = g_devices;
    while (tail->next)
      tail = tail->next;
    tail->next = bdev;
  }
  g_device_count++;

  uint64_t size_mb = bdev_size_mb(bdev);
  LOG_INFO("[BLK] Registrado %s: %lu sectores de %u bytes (%lu MB)%s%s",
           bdev->name, (unsigned long)bdev->num_sectors, bdev->sector_size,
           (unsigned long)size_mb, bdev->is_read_only ? " [RO]" : "",
           bdev->is_partition ? " [partición]" : "");

  return 0;
}

int blk_unregister(block_device_t *bdev) {
  if (!bdev)
    return -EINVAL;

  block_device_t **pp = &g_devices;
  while (*pp) {
    if (*pp == bdev) {
      *pp = bdev->next;
      bdev->next = NULL;
      g_device_count--;
      LOG_INFO("[BLK] Desregistrado %s", bdev->name);
      return 0;
    }
    pp = &(*pp)->next;
  }
  return -ENOENT;
}

// ---------------------------------------------------------------------------
// Búsqueda
// ---------------------------------------------------------------------------
block_device_t *blk_lookup(const char *name) {
  if (!name)
    return NULL;
  for (block_device_t *p = g_devices; p; p = p->next) {
    if (strcmp(p->name, name) == 0)
      return p;
  }
  return NULL;
}

block_device_t *blk_get_by_index(int index) {
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

int blk_count(void) { return g_device_count; }

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
int blk_submit(bio_t *bio) {
  if (!bio || !bio->bdev) {
    LOG_ERR("[BLK] submit: bio inválido");
    return -EINVAL;
  }
  block_device_t *bdev = bio->bdev;
  if (!bdev->ops || !bdev->ops->submit) {
    LOG_ERR("[BLK] submit: %s sin ops->submit", bdev->name);
    return -EINVAL;
  }
  return bdev->ops->submit(bdev, bio);
}

void bio_endio(bio_t *bio, int error) {
  if (!bio)
    return;
  bio->error = error;
  if (bio->end_io) {
    bio->end_io(bio);
  }
}

// ---------------------------------------------------------------------------
// Wrappers síncronos (bdev_*)
//
// En Fase 1, todos los drivers son síncronos: ops->submit devuelve cuando
// el bio ha terminado. Así que no hace falta completion.
//
// En Fase 3 (DMA), estos wrappers cambiarán a usar completion:
//   - inicializan una completion,
//   - ponen bio->end_io = completion_callback,
//   - llaman a blk_submit,
//   - esperan con wait_for_completion.
// La estructura del bio ya lo permite.
// ---------------------------------------------------------------------------

static int validate_io(block_device_t *bdev, uint64_t lba, uint32_t count) {
  if (count == 0)
    return -EINVAL;
  if (lba > bdev->num_sectors)
    return -ERANGE;
  if (count > bdev->num_sectors - lba)
    return -ERANGE;
  return 0;
}

int bdev_read(block_device_t *bdev, uint64_t lba, uint32_t count, void *buf) {
  if (!bdev || !buf)
    return -EINVAL;

  int rc = validate_io(bdev, lba, count);
  if (rc < 0)
    return rc;

  bio_t bio = {
      .bdev = bdev,
      .lba = lba,
      .count = count,
      .buf = buf,
      .op = BIO_READ,
      .error = 0,
      .end_io = NULL,
      .end_io_data = NULL,
      .next = NULL,
  };

  // [DEBUG RBP] Capturar RBP justo antes de la llamada.
  uint64_t rbp_before;
  __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_before));

  rc = bdev_submit_sync(&bio);

  // [DEBUG RBP] Verificar que RBP no cambió durante la llamada.
  uint64_t rbp_after;
  __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_after));
  if (rbp_before != rbp_after) {
    LOG_ERR("[BDEV_READ] RBP cambió: %p -> %p (lba=%llu count=%u)",
            (void *)rbp_before, (void *)rbp_after, (unsigned long long)lba,
            count);
    // Restaurar antes del leave, si no crasheamos en el próximo frame.
    __asm__ volatile("mov %0, %%rbp" : : "r"(rbp_before));
  }

  return rc;
}

int bdev_write(block_device_t *bdev, uint64_t lba, uint32_t count,
               const void *buf) {
  if (!bdev || !buf)
    return -EINVAL;
  if (bdev->is_read_only)
    return -EROFS;

  int rc = validate_io(bdev, lba, count);
  if (rc < 0)
    return rc;

  bio_t bio = {
      .bdev = bdev,
      .lba = lba,
      .count = count,
      .buf = (void *)buf,
      .op = BIO_WRITE,
      .error = 0,
      .end_io = NULL,
      .end_io_data = NULL,
      .next = NULL,
  };

  // [DEBUG RBP] Capturar RBP justo antes de la llamada.
  uint64_t rbp_before;
  __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_before));

  rc = bdev_submit_sync(&bio);

  // [DEBUG RBP] Verificar que RBP no cambió durante la llamada.
  uint64_t rbp_after;
  __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_after));
  if (rbp_before != rbp_after) {
    LOG_ERR("[BDEV_WRITE] RBP cambió: %p -> %p (lba=%llu count=%u)",
            (void *)rbp_before, (void *)rbp_after, (unsigned long long)lba,
            count);
    // Restaurar antes del leave.
    __asm__ volatile("mov %0, %%rbp" : : "r"(rbp_before));
  }

  return rc;
}

int bdev_flush(block_device_t *bdev) {
  if (!bdev)
    return -EINVAL;
  if (bdev->is_read_only)
    return 0;
  if (!bdev->ops || !bdev->ops->flush)
    return 0;

  bio_t bio = {
      .bdev = bdev,
      .lba = 0,
      .count = 0,
      .buf = NULL,
      .op = BIO_FLUSH,
      .error = 0,
      .end_io = NULL,
      .end_io_data = NULL,
      .next = NULL,
  };

  return bdev_submit_sync(&bio);
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
void blk_dump(void) {
  LOG_INFO("[BLK] Discos registrados (%d):", g_device_count);
  for (block_device_t *p = g_devices; p; p = p->next) {
    LOG_INFO("  %s: %lu sectores, %u bytes/sector, %lu MB%s%s", p->name,
             (unsigned long)p->num_sectors, p->sector_size,
             (unsigned long)bdev_size_mb(p), p->is_read_only ? " [RO]" : "",
             p->is_partition ? " [partición]" : "");
    if (p->ops && p->ops->dump) {
      p->ops->dump(p);
    }
  }
}