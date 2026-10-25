// kernel/block_proxy.c
//
// [FASE 5.1] Puente entre el block layer y un driver de bloque en Ring 3.
//
// Un .aur con AUR_CAP_BLOCK_DEVICE abre /dev/blkproxy, registra sus discos
// con BLKP_REGISTER (aparecen como sdX en el block layer) y sirve las
// peticiones con BLKP_FETCH / BLKP_COMPLETE. Ver el contrato en aur.h.
//
//   FS / part / bdev_read
//        ↓ blk_submit
//   blkp_submit  ──encola──►  pending ──BLKP_FETCH──► driver .aur
//        ▲                                                │
//        └──────── bio_endio ◄── BLKP_COMPLETE ◄──────────┘
//
// El submit es ASÍNCRONO (-EINPROGRESS); bio_endio se llama desde el
// contexto del proceso driver. Los bios grandes se trocean en peticiones
// de max_sectors; el bio termina cuando se completa el último trozo.
//
// [FASE 6.1] Cada petición lleva un plazo (BLKP_OP_TIMEOUT_MS). Un watchdog
// del kernel, activo mientras el driver está conectado, falla con -ETIMEDOUT
// las que lo superan (pendientes de entregar o entregadas sin completar), de
// modo que un driver colgado no deja al FS bloqueado para siempre. Un
// BLKP_COMPLETE tardío recibe -ENOENT.
//
// Si el driver muere/cierra el fd, todo lo pendiente falla con -EIO y los
// discos se dan de baja. La memoria del block_device_t NO se libera: las
// particiones hijas (part.c) y los montajes pueden seguir apuntando a ella
// y deben recibir -ENODEV, no un use-after-free.

#include "block_proxy.h"
#include "aur.h"
#include "block.h"
#include "completion.h"
#include "heap.h"
#include "klog.h"
#include "kthread.h"
#include "part.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "test.h"
#include "uaccess.h"
#include "wait.h"

// 1 tick = 1 ms (ver sched_get_ticks). 30 s cubre spin-up de discos lentos.
#define BLKP_OP_TIMEOUT_MS 30000u
#define BLKP_WATCHDOG_PERIOD_MS 1000u

#define BLKP_MAX_DISKS 8
#define BLKP_DEFAULT_MAX_SECTORS 128
#define BLKP_MAX_XFER_BYTES (1024u * 1024u)

typedef struct blkp_disk {
  int used;  // slot reservado
  int alive; // acepta bios
  uint32_t id;
  uint32_t max_sectors;
  block_device_t bdev;
} blkp_disk_t;

typedef struct blkp_io {
  struct blkp_io *next;
  bio_t *bio;
  blkp_disk_t *disk;
  uint32_t done;  // sectores ya completados
  uint32_t chunk; // sectores de la petición en vuelo
  uint64_t tag;
  uint64_t deadline; // tick límite; se renueva en cada cambio de cola
} blkp_io_t;

static blkp_disk_t g_disks[BLKP_MAX_DISKS];
static blkp_io_t *g_pend_head, *g_pend_tail; // aún no entregadas al driver
static blkp_io_t *g_inflight;                // entregadas, sin completar
static spinlock_t g_lock = {.locked = 0};
static wait_queue_t g_wq;
static completion_t g_ready;
static uint64_t g_next_tag;
static int g_open;
static vfs_node_t *g_owner; // nodo VFS que hizo el open con éxito
static int g_boot_scanned;
static int g_init_done;
static int g_wd_running; // hay un watchdog vivo (protegido por g_lock)
static uint32_t g_owner_pid; // pid del .aur conectado
static int g_wd_killed;      // ya se mató al driver de esta conexión
static wait_queue_t g_wd_wq;

// ---------------------------------------------------------------------------
// Utilidades de cola (llamar con g_lock tomado)
// ---------------------------------------------------------------------------
static void pend_push(blkp_io_t *io) {
  io->next = NULL;
  if (g_pend_tail)
    g_pend_tail->next = io;
  else
    g_pend_head = io;
  g_pend_tail = io;
}

static blkp_io_t *pend_pop(void) {
  blkp_io_t *io = g_pend_head;
  if (io) {
    g_pend_head = io->next;
    if (!g_pend_head)
      g_pend_tail = NULL;
    io->next = NULL;
  }
  return io;
}

static void inflight_add(blkp_io_t *io) {
  io->next = g_inflight;
  g_inflight = io;
}

static blkp_io_t *inflight_remove(uint64_t tag) {
  for (blkp_io_t **pp = &g_inflight; *pp; pp = &(*pp)->next) {
    if ((*pp)->tag == tag) {
      blkp_io_t *io = *pp;
      *pp = io->next;
      io->next = NULL;
      return io;
    }
  }
  return NULL;
}

static void blkp_finish(blkp_io_t *io, int error) {
  bio_t *bio = io->bio;
  kfree(io);
  bio_endio(bio, error); // el bio puede dejar de existir tras esta llamada
}

static void io_arm(blkp_io_t *io) {
  io->deadline = sched_get_ticks() + BLKP_OP_TIMEOUT_MS;
}

// Falla con -ETIMEDOUT las peticiones cuyo plazo venció antes de `now`.
// Devuelve cuántas.
static int blkp_reap_expired(uint64_t now) {
  blkp_io_t *failed = NULL;

  unsigned long fl = spin_lock_irqsave(&g_lock);
  blkp_io_t *prev = NULL;
  for (blkp_io_t *io = g_pend_head, *nx; io; io = nx) {
    nx = io->next;
    if (now > io->deadline) {
      if (prev)
        prev->next = nx;
      else
        g_pend_head = nx;
      if (g_pend_tail == io)
        g_pend_tail = prev;
      io->next = failed;
      failed = io;
    } else {
      prev = io;
    }
  }
  for (blkp_io_t **pp = &g_inflight; *pp;) {
    blkp_io_t *io = *pp;
    if (now > io->deadline) {
      *pp = io->next;
      io->next = failed;
      failed = io;
    } else {
      pp = &io->next;
    }
  }
  spin_unlock_irqrestore(&g_lock, fl);

  int n = 0;
  for (blkp_io_t *io = failed, *nx; io; io = nx) {
    nx = io->next;
    blkp_finish(io, -ETIMEDOUT);
    n++;
  }
  if (n)
    LOG_ERR("[BLKP] %d peticiones sin respuesta en %u ms: -ETIMEDOUT", n,
            BLKP_OP_TIMEOUT_MS);
  return n;
}

// [FASE 6.2] Un driver que deja vencer peticiones está colgado: se le mata
// (una vez por conexión) para que el supervisor de aur_bus lo relance (6.3).
static void blkp_kill_hung_driver(void) {
  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (!g_open || g_wd_killed) {
    spin_unlock_irqrestore(&g_lock, fl);
    return;
  }
  g_wd_killed = 1;
  uint32_t pid = g_owner_pid;
  spin_unlock_irqrestore(&g_lock, fl);
  LOG_ERR("[BLKP] driver pid=%u no responde: SIGKILL", pid);
  process_kill(pid, SIGKILL);
}

static bool blkp_wd_never(void *arg) {
  (void)arg;
  return false;
}

// Corre mientras el driver está conectado. Sale (y limpia g_wd_running bajo
// g_lock, atómico respecto a blkp_open) cuando se desconecta.
static int blkp_watchdog(void *arg) {
  (void)arg;
  for (;;) {
    wait_event_timeout(&g_wd_wq, blkp_wd_never, NULL, BLKP_WATCHDOG_PERIOD_MS);

    unsigned long fl = spin_lock_irqsave(&g_lock);
    if (!g_open) {
      g_wd_running = 0;
      spin_unlock_irqrestore(&g_lock, fl);
      return 0;
    }
    spin_unlock_irqrestore(&g_lock, fl);
    if (blkp_reap_expired(sched_get_ticks()) > 0)
      blkp_kill_hung_driver();
  }
}

// ---------------------------------------------------------------------------
// block_ops_t
// ---------------------------------------------------------------------------
static int blkp_submit(block_device_t *bdev, bio_t *bio) {
  blkp_disk_t *d = (blkp_disk_t *)bdev->private_data;
  if (!d || !d->alive)
    return -ENODEV;

  if (bio->op != BIO_FLUSH) {
    if (bio->count == 0 || bio->lba > bdev->num_sectors ||
        bio->count > bdev->num_sectors - bio->lba)
      return -ERANGE;
    if (bio->op == BIO_WRITE && bdev->is_read_only)
      return -EROFS;
  }

  blkp_io_t *io = (blkp_io_t *)kzalloc(sizeof(*io));
  if (!io)
    return -ENOMEM;
  io->bio = bio;
  io->disk = d;
  bio->error = 0;

  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (!g_open || !d->alive) {
    spin_unlock_irqrestore(&g_lock, fl);
    kfree(io);
    return -ENODEV;
  }
  io_arm(io);
  pend_push(io);
  spin_unlock_irqrestore(&g_lock, fl);

  wake_up_all(&g_wq);
  return -EINPROGRESS;
}

// bdev_flush() solo emite un BIO_FLUSH si ops->flush != NULL; el trabajo
// real lo hace el driver al recibir AUR_BLK_OP_FLUSH.
static void blkp_flush_stub(block_device_t *bdev) { (void)bdev; }

static void blkp_dump(block_device_t *bdev) {
  blkp_disk_t *d = (blkp_disk_t *)bdev->private_data;
  if (d)
    LOG_INFO("    [BLKP] disk_id=%u max_sectors=%u alive=%d", d->id,
             d->max_sectors, d->alive);
}

static block_ops_t g_blkp_ops = {
    .submit = blkp_submit,
    .flush = blkp_flush_stub,
    .dump = blkp_dump,
};

// ---------------------------------------------------------------------------
// ioctl: BLKP_REGISTER
// ---------------------------------------------------------------------------
static void blkp_make_name(int idx, char *out) {
  out[0] = 's';
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

static int64_t blkp_register(uint64_t arg) {
  struct aur_blk_register r;
  if (copy_from_user(&r, (void *)arg, sizeof(r)) < 0)
    return -EFAULT;

  uint32_t ss = r.sector_size;
  if (r.num_sectors == 0 || ss < 512 || ss > 4096 || (ss & (ss - 1)) != 0)
    return -EINVAL;
  uint32_t maxs = r.max_sectors ? r.max_sectors : BLKP_DEFAULT_MAX_SECTORS;
  if (maxs > BLKP_MAX_XFER_BYTES / ss)
    maxs = BLKP_MAX_XFER_BYTES / ss;

  blkp_disk_t *d = NULL;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  for (int i = 0; i < BLKP_MAX_DISKS; i++) {
    if (!g_disks[i].used) {
      d = &g_disks[i];
      memset(d, 0, sizeof(*d));
      d->used = 1;
      d->id = (uint32_t)i;
      break;
    }
  }
  spin_unlock_irqrestore(&g_lock, fl);
  if (!d)
    return -ENOSPC;

  char name[8];
  int idx;
  for (idx = 0; idx < 26 * 27; idx++) {
    blkp_make_name(idx, name);
    if (!blk_lookup(name))
      break;
  }
  if (idx == 26 * 27) {
    d->used = 0;
    return -ENOSPC;
  }

  strcpy(d->bdev.name, name);
  d->bdev.num_sectors = r.num_sectors;
  d->bdev.sector_size = ss;
  d->bdev.is_read_only = (r.flags & AUR_BLK_RO) ? 1 : 0;
  d->bdev.ops = &g_blkp_ops;
  d->bdev.private_data = d;
  d->max_sectors = maxs;
  d->alive = 1; // antes de blk_register: pueden llegar bios al instante

  int rc = blk_register(&d->bdev);
  if (rc < 0) {
    d->alive = 0;
    d->used = 0;
    return rc;
  }

  memset(r.name, 0, sizeof(r.name));
  strcpy(r.name, name);
  r.max_sectors = maxs;
  r.disk_id = d->id;
  if (copy_to_user((void *)arg, &r, sizeof(r)) < 0) {
    // El driver no sabrá su disk_id: dar de baja.
    d->alive = 0;
    blk_unregister(&d->bdev);
    return -EFAULT;
  }

  LOG_INFO("[BLKP] disco %s registrado (id=%u, %lu sectores de %u B, "
           "max_sectors=%u)",
           name, d->id, (unsigned long)r.num_sectors, ss, maxs);
  return 0;
}

// ---------------------------------------------------------------------------
// ioctl: BLKP_READY
// ---------------------------------------------------------------------------
static int blkp_scan_task(void *arg) {
  (void)arg;
  part_scan_all();
  return 0;
}

static int64_t blkp_ready(void) {
  complete_all(&g_ready);
  if (__atomic_load_n(&g_boot_scanned, __ATOMIC_ACQUIRE))
    kthread_run(blkp_scan_task, NULL, "blkp-scan");
  return 0;
}

// ---------------------------------------------------------------------------
// ioctl: BLKP_FETCH
// ---------------------------------------------------------------------------
static bool blkp_has_work(void *arg) {
  (void)arg;
  return g_pend_head != NULL;
}

static int64_t blkp_fetch(uint64_t arg) {
  struct aur_blk_fetch f;
  if (copy_from_user(&f, (void *)arg, sizeof(f)) < 0)
    return -EFAULT;

  blkp_io_t *io;
  uint32_t ss;
  uint64_t nbytes = 0;

  for (;;) {
    unsigned long fl = spin_lock_irqsave(&g_lock);
    io = g_pend_head;
    if (io) {
      ss = io->disk->bdev.sector_size;
      bio_t *bio = io->bio;
      uint32_t chunk = 0;
      if (bio->op != BIO_FLUSH) {
        chunk = bio->count - io->done;
        if (chunk > io->disk->max_sectors)
          chunk = io->disk->max_sectors;
      }
      nbytes = (uint64_t)chunk * ss;
      if (bio->op == BIO_WRITE && f.buf_size < nbytes) {
        spin_unlock_irqrestore(&g_lock, fl);
        return -EMSGSIZE; // sin sacar de la cola: reintentar con más buffer
      }
      pend_pop();
      io->chunk = chunk;
      io->tag = ++g_next_tag;
      io_arm(io);
      inflight_add(io);
      spin_unlock_irqrestore(&g_lock, fl);
      break;
    }
    spin_unlock_irqrestore(&g_lock, fl);

    if (f.flags & AUR_BLK_NOWAIT)
      return -EAGAIN;
    int rc = wait_event_interruptible(&g_wq, blkp_has_work, NULL);
    if (rc < 0)
      return rc;
  }

  bio_t *bio = io->bio;
  memset(&f.req, 0, sizeof(f.req));
  f.req.tag = io->tag;
  f.req.lba = bio->lba + io->done;
  f.req.disk_id = io->disk->id;
  f.req.count = io->chunk;
  f.req.op = (bio->op == BIO_READ)    ? AUR_BLK_OP_READ
             : (bio->op == BIO_WRITE) ? AUR_BLK_OP_WRITE
                                      : AUR_BLK_OP_FLUSH;

  int err = 0;
  if (bio->op == BIO_WRITE &&
      copy_to_user((void *)f.buf, (uint8_t *)bio->buf + (uint64_t)io->done * ss,
                   nbytes) < 0)
    err = -EFAULT;
  if (!err && copy_to_user((void *)arg, &f, sizeof(f)) < 0)
    err = -EFAULT;

  if (err) {
    // El driver no verá esta petición: abortarla para no colgar al llamante.
    unsigned long fl = spin_lock_irqsave(&g_lock);
    blkp_io_t *rm = inflight_remove(io->tag);
    spin_unlock_irqrestore(&g_lock, fl);
    if (rm)
      blkp_finish(rm, -EIO);
    return err;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// ioctl: BLKP_COMPLETE
// ---------------------------------------------------------------------------
static int64_t blkp_complete(uint64_t arg) {
  struct aur_blk_done dn;
  if (copy_from_user(&dn, (void *)arg, sizeof(dn)) < 0)
    return -EFAULT;

  unsigned long fl = spin_lock_irqsave(&g_lock);
  blkp_io_t *io = inflight_remove(dn.tag);
  spin_unlock_irqrestore(&g_lock, fl);
  if (!io)
    return -ENOENT;

  bio_t *bio = io->bio;
  if (dn.error != 0) {
    blkp_finish(io, dn.error < 0 ? dn.error : -EIO);
    return 0;
  }

  if (bio->op == BIO_READ) {
    uint32_t ss = io->disk->bdev.sector_size;
    uint64_t nbytes = (uint64_t)io->chunk * ss;
    if (dn.buf_size < nbytes) {
      blkp_finish(io, -EIO);
      return -EMSGSIZE;
    }
    if (copy_from_user((uint8_t *)bio->buf + (uint64_t)io->done * ss,
                       (void *)dn.buf, nbytes) < 0) {
      blkp_finish(io, -EFAULT);
      return -EFAULT;
    }
  }

  io->done += io->chunk;
  if (bio->op == BIO_FLUSH || io->done >= bio->count) {
    blkp_finish(io, 0);
    return 0;
  }

  fl = spin_lock_irqsave(&g_lock);
  io_arm(io);
  pend_push(io);
  spin_unlock_irqrestore(&g_lock, fl);
  wake_up_all(&g_wq);
  return 0;
}

// ---------------------------------------------------------------------------
// VFS: open / close / ioctl / poll
// ---------------------------------------------------------------------------
static int blkp_open(vfs_node_t *node, int flags) {
  (void)flags;
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;
  if (!(proc->aur.caps & AUR_CAP_BLOCK_DEVICE)) {
    LOG_WARN("[BLKP] pid=%u sin AUR_CAP_BLOCK_DEVICE (caps=0x%x)", proc->pid,
             proc->aur.caps);
    return -EPERM;
  }

  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (g_open) {
    spin_unlock_irqrestore(&g_lock, fl);
    return -EBUSY;
  }
  g_open = 1;
  g_owner = node;
  g_owner_pid = proc->pid;
  g_wd_killed = 0;
  int start_wd = !g_wd_running;
  g_wd_running = 1;
  spin_unlock_irqrestore(&g_lock, fl);
  if (start_wd && !kthread_run(blkp_watchdog, NULL, "blkp-wd")) {
    fl = spin_lock_irqsave(&g_lock);
    g_wd_running = 0;
    spin_unlock_irqrestore(&g_lock, fl);
    LOG_WARN("[BLKP] sin watchdog de peticiones (kthread_run falló)");
  }
  LOG_INFO("[BLKP] driver de bloque pid=%u conectado", proc->pid);
  return 0;
}

static int blkp_close(vfs_node_t *node) {
  // El VFS llama a close() al destruir CUALQUIER nodo (stat, open fallido
  // por EBUSY...). Solo el nodo que hizo el open con éxito desconecta.
  blkp_io_t *failed = NULL;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (!g_open || node != g_owner) {
    spin_unlock_irqrestore(&g_lock, fl);
    return 0;
  }
  g_open = 0;
  g_owner = NULL;
  for (blkp_io_t *io = g_pend_head, *nx; io; io = nx) {
    nx = io->next;
    io->next = failed;
    failed = io;
  }
  g_pend_head = g_pend_tail = NULL;
  for (blkp_io_t *io = g_inflight, *nx; io; io = nx) {
    nx = io->next;
    io->next = failed;
    failed = io;
  }
  g_inflight = NULL;
  for (int i = 0; i < BLKP_MAX_DISKS; i++)
    g_disks[i].alive = 0;
  spin_unlock_irqrestore(&g_lock, fl);

  int n = 0;
  for (blkp_io_t *io = failed, *nx; io; io = nx) {
    nx = io->next;
    blkp_finish(io, -EIO);
    n++;
  }

  for (int i = 0; i < BLKP_MAX_DISKS; i++) {
    if (g_disks[i].used) {
      blk_unregister(&g_disks[i].bdev);
      g_disks[i].used = 0; // la memoria queda (ver cabecera)
    }
  }
  LOG_WARN("[BLKP] driver de bloque desconectado (%d bios abortados)", n);
  return 0;
}

static int64_t blkp_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  (void)node;
  switch (req) {
  case BLKP_REGISTER:
    return blkp_register(arg);
  case BLKP_READY:
    return blkp_ready();
  case BLKP_FETCH:
    return blkp_fetch(arg);
  case BLKP_COMPLETE:
    return blkp_complete(arg);
  default:
    return -ENOTTY;
  }
}

static int blkp_poll(vfs_node_t *node, short events) {
  (void)node;
  int rev = 0;
  if ((events & 1) && g_pend_head)
    rev |= 1; // POLLIN: hay peticiones
  return rev;
}

static wait_queue_t *blkp_poll_wq(vfs_node_t *node) {
  (void)node;
  return &g_wq;
}

vfs_ops_t blkp_dev_ops = {
    .open = blkp_open,
    .close = blkp_close,
    .ioctl = blkp_ioctl,
    .poll = blkp_poll,
    .poll_wq = blkp_poll_wq,
};

// ---------------------------------------------------------------------------
// Arranque
// ---------------------------------------------------------------------------
void blkp_init(void) {
  if (__atomic_exchange_n(&g_init_done, 1, __ATOMIC_ACQ_REL))
    return;
  wait_queue_init(&g_wq);
  wait_queue_init(&g_wd_wq);
  completion_init(&g_ready);
}

int blkp_wait_ready(uint32_t timeout_ms) {
  return wait_for_completion_timeout(&g_ready, timeout_ms) != 0;
}

void blkp_boot_scanned(void) {
  __atomic_store_n(&g_boot_scanned, 1, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void test_end_io(bio_t *bio) { bio->end_io_data = (void *)1; }

// El watchdog falla (y solo falla) las peticiones vencidas, tanto pendientes
// como en vuelo, y deja las colas coherentes.
static void test_blkp_reap_expired(void) {
  unsigned long fl = spin_lock_irqsave(&g_lock);
  int busy = g_open || g_pend_head || g_inflight;
  spin_unlock_irqrestore(&g_lock, fl);
  if (busy) {
    test_skip("blkproxy en uso");
    return;
  }

  bio_t b_old = {.end_io = test_end_io}, b_new = {.end_io = test_end_io};
  bio_t b_fly = {.end_io = test_end_io};
  blkp_io_t *old = kzalloc(sizeof(*old)), *young = kzalloc(sizeof(*young));
  blkp_io_t *fly = kzalloc(sizeof(*fly));
  TEST_ASSERT(old && young && fly, "kzalloc");
  old->bio = &b_old;
  old->deadline = 1000;
  young->bio = &b_new;
  young->deadline = 5000;
  fly->bio = &b_fly;
  fly->deadline = 1000;
  fly->tag = 42;

  fl = spin_lock_irqsave(&g_lock);
  pend_push(old);
  pend_push(young);
  inflight_add(fly);
  spin_unlock_irqrestore(&g_lock, fl);

  TEST_ASSERT_EQ(blkp_reap_expired(1000), 0); // plazo no superado aún
  TEST_ASSERT_EQ(blkp_reap_expired(1001), 2); // old + fly
  TEST_ASSERT(b_old.end_io_data && b_fly.end_io_data, "bios vencidos cerrados");
  TEST_ASSERT(b_old.error == -ETIMEDOUT && b_fly.error == -ETIMEDOUT,
              "error ETIMEDOUT");
  TEST_ASSERT(!b_new.end_io_data, "bio joven intacto");
  TEST_ASSERT(g_pend_head == young && g_pend_tail == young, "cola pendiente");
  TEST_ASSERT(g_inflight == NULL, "sin vuelo residual");

  TEST_ASSERT_EQ(blkp_reap_expired(5001), 1);
  TEST_ASSERT(g_pend_head == NULL && g_pend_tail == NULL, "cola vacía");
}
REGISTER_TEST("blkproxy: watchdog expira peticiones", test_blkp_reap_expired);
