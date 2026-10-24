// kernel/irq_user.c
//
// [FASE 4.5] Primitivas IRQ para userland drivers (.aur).
//
// El .aur abre /dev/irq/<N> (requiere AUR_CAP_IRQ). Puede:
//   - leer: bloquea hasta que llegue una IRQ; devuelve {irq, count=8B}
//   - poll/epoll: POLLIN cuando count > 0
//   - ioctl(AUR_IRQ_MASK/UNMASK): enmascara/desenmascara en IOAPIC
//     (solo IRQs legacy; MSI es edge-triggered y no lo necesita)
//   - close: libera el registro si era el último fd
//
// Internamente delega en irq_threaded:
//   - ack    → aur_irq_ack_fn  (hard IRQ)   → IRQ_WAKE_THREAD
//   - thread → aur_irq_thread_fn(kthread)   → inc count + wake_up_all
//
// El ack NO toca el device: el .aur es quien lee registros MMIO y ackea
// el hardware (patrón estándar de drivers reales). El .aur debe mantener
// el bit "interrupt enable" del device apagado hasta estar listo.

#include "irq_user.h"
#include "apic.h"
#include "aur.h"
#include "idt.h"
#include "irq_threaded.h"
#include "klog.h"
#include "process.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include "wait.h"

// ---------------------------------------------------------------------------
// Estado por IRQ
// ---------------------------------------------------------------------------
struct aur_irq_ctx {
  spinlock_t lock;         // protege registered/refcount
  wait_queue_t wq;         // waiters de read() y epoll
  volatile uint32_t count; // IRQs desde el último read()
  int registered;          // 1 = irq_request_threaded() OK
  int refcount;            // fds abiertos
  uint8_t irq;
};

static struct aur_irq_ctx g_irq_ctxs[IRQ_USER_MAX];
static int g_aur_irq_init_done = 0;
static spinlock_t g_aur_irq_init_lock = {.locked = 0};

static struct aur_irq_ctx *aur_irq_get_ctx(uint32_t irq) {
  if (irq >= IRQ_USER_MAX)
    return NULL;
  // Solo legacy (0..15) y MSI (0x60..0x6F).
  if (!(irq <= 15 || (irq >= MSI_VECTOR_BASE && irq <= MSI_VECTOR_END)))
    return NULL;
  return &g_irq_ctxs[irq];
}

// ---------------------------------------------------------------------------
// Callbacks de irq_threaded
// ---------------------------------------------------------------------------
static irqreturn_t aur_irq_ack_fn(int irq, void *dev_id) {
  (void)irq;
  (void)dev_id;
  // No tocamos el device: el .aur lo hará por MMIO. Solo pedimos al
  // kthread que despierte a los waiters.
  return IRQ_WAKE_THREAD;
}

static irqreturn_t aur_irq_thread_fn(int irq, void *dev_id) {
  (void)irq;
  struct aur_irq_ctx *ctx = (struct aur_irq_ctx *)dev_id;
  __atomic_fetch_add(&ctx->count, 1, __ATOMIC_ACQ_REL);
  wake_up_all(&ctx->wq);
  return IRQ_HANDLED;
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------
static int aur_irq_open(vfs_node_t *node, int flags) {
  (void)flags;
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;
  if (!(proc->aur.caps & AUR_CAP_IRQ)) {
    LOG_WARN("[IRQ-USER] pid=%u sin AUR_CAP_IRQ (caps=0x%x)", proc->pid,
             proc->aur.caps);
    return -EPERM;
  }

  struct aur_irq_ctx *ctx = (struct aur_irq_ctx *)node->priv;
  if (!ctx)
    return -EINVAL;

  unsigned long lf = spin_lock_irqsave(&ctx->lock);
  if (!ctx->registered) {
    // Primer open: registrar en irq_threaded.
    char name[16];
    int p = 0;
    const char *pre = "uirq";
    while (*pre && p < 15)
      name[p++] = *pre++;
    char buf[8];
    int n = 0;
    uint32_t v = ctx->irq;
    if (v == 0)
      buf[n++] = '0';
    while (v > 0) {
      buf[n++] = (char)('0' + (v % 10));
      v /= 10;
    }
    while (n > 0 && p < 15)
      name[p++] = buf[--n];
    name[p] = '\0';

    int rc = irq_request_threaded(ctx->irq, aur_irq_ack_fn, aur_irq_thread_fn,
                                  name, ctx);
    if (rc != 0) {
      spin_unlock_irqrestore(&ctx->lock, lf);
      LOG_WARN("[IRQ-USER] irq=%u: irq_request_threaded falló: %d", ctx->irq,
               rc);
      return rc;
    }

    // [FIX] Enrutar en el IOAPIC si es una IRQ legacy (0..15).
    // Las funciones ya están declaradas en apic.h.
    if (ctx->irq <= 15) {
      ioapic_redirect_irq_ex(ctx->irq, 32 + ctx->irq, lapic_get_bsp_id(), 0, 1,
                             1);
    }

    ctx->registered = 1;
    __atomic_store_n(&ctx->count, 0, __ATOMIC_RELEASE);
    LOG_INFO("[IRQ-USER] irq=%u registrada (pid=%u)", ctx->irq, proc->pid);
  }
  ctx->refcount++;
  spin_unlock_irqrestore(&ctx->lock, lf);
  return 0;
}

static int aur_irq_close(vfs_node_t *node) {
  struct aur_irq_ctx *ctx = (struct aur_irq_ctx *)node->priv;
  if (!ctx)
    return 0;

  unsigned long lf = spin_lock_irqsave(&ctx->lock);
  ctx->refcount--;
  int do_free = 0;
  if (ctx->refcount <= 0) {
    ctx->refcount = 0;
    if (ctx->registered) {
      ctx->registered = 0;
      do_free = 1;
    }
  }
  spin_unlock_irqrestore(&ctx->lock, lf);

  if (do_free) {
    // irq_free bloquea hasta que el kthread de irq_threaded termina la
    // iteración en curso. Siempre llamada desde close() (task context).
    irq_free(ctx->irq);
    LOG_INFO("[IRQ-USER] irq=%u liberada", ctx->irq);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// read / poll
// ---------------------------------------------------------------------------
static bool aur_irq_ready(void *arg) {
  struct aur_irq_ctx *ctx = (struct aur_irq_ctx *)arg;
  return __atomic_load_n(&ctx->count, __ATOMIC_ACQUIRE) > 0;
}

static int64_t aur_irq_read(vfs_node_t *node, uint64_t offset, size_t size,
                            void *buf) {
  (void)offset;
  struct aur_irq_ctx *ctx = (struct aur_irq_ctx *)node->priv;
  if (!ctx)
    return -EINVAL;
  if (size < sizeof(struct aur_irq_event))
    return -EINVAL;
  if (!buf)
    return -EFAULT;

  while (1) {
    int rc = wait_event_interruptible(&ctx->wq, aur_irq_ready, ctx);
    if (rc < 0)
      return rc;

    // Intentar consumir el contador. Si dos waiters compiten, solo uno
    // gana el exchange (el otro recibe 0 y vuelve a dormir).
    uint32_t n = __atomic_exchange_n(&ctx->count, 0, __ATOMIC_ACQ_REL);
    if (n == 0)
      continue;

    struct aur_irq_event ev = {
        .irq = ctx->irq,
        .count = n,
    };
    memcpy(buf, &ev, sizeof(ev));
    return (int64_t)sizeof(ev);
  }
}

static int aur_irq_poll(vfs_node_t *node, short events) {
  struct aur_irq_ctx *ctx = (struct aur_irq_ctx *)node->priv;
  if (!ctx)
    return 0;
  int rev = 0;
  if ((events & 1) && __atomic_load_n(&ctx->count, __ATOMIC_ACQUIRE) > 0)
    rev |= 1;
  return rev;
}

static wait_queue_t *aur_irq_poll_wq(vfs_node_t *node) {
  struct aur_irq_ctx *ctx = (struct aur_irq_ctx *)node->priv;
  return ctx ? &ctx->wq : NULL;
}

// ---------------------------------------------------------------------------
// ioctl (mask/unmask en IOAPIC, solo legacy)
// ---------------------------------------------------------------------------
static int64_t aur_irq_ioctl(vfs_node_t *node, unsigned long req,
                             uint64_t arg) {
  (void)arg;
  struct aur_irq_ctx *ctx = (struct aur_irq_ctx *)node->priv;
  if (!ctx)
    return -EINVAL;

  switch (req) {
  case AUR_IRQ_MASK:
    if (ctx->irq <= 15)
      ioapic_mask_irq(ctx->irq, 1);
    return 0;
  case AUR_IRQ_UNMASK:
    if (ctx->irq <= 15)
      ioapic_mask_irq(ctx->irq, 0);
    return 0;
  default:
    return -ENOTTY;
  }
}

struct vfs_ops aur_irq_node_ops = {
    .open = aur_irq_open,
    .close = aur_irq_close,
    .read = aur_irq_read,
    .poll = aur_irq_poll,
    .poll_wq = aur_irq_poll_wq,
    .ioctl = aur_irq_ioctl,
};

// ---------------------------------------------------------------------------
// readdir de /dev/irq: 0..15 legacy + 0x60..0x6F MSI
// ---------------------------------------------------------------------------
static int aur_irq_dir_readdir(vfs_node_t *dir, uint64_t index,
                               vfs_dirent_t *out) {
  (void)dir;
  if (!out)
    return -EINVAL;

  const uint32_t TOTAL = 16u + (MSI_VECTOR_END - MSI_VECTOR_BASE + 1u);
  if (index >= TOTAL) {
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
    return 0;
  }

  uint32_t irq;
  if (index < 16)
    irq = (uint32_t)index;
  else
    irq = MSI_VECTOR_BASE + (uint32_t)(index - 16);

  // Formatear decimal.
  char tmp[12];
  int t = 0;
  uint32_t v = irq;
  if (v == 0)
    tmp[t++] = '0';
  while (v > 0) {
    tmp[t++] = (char)('0' + (v % 10));
    v /= 10;
  }
  int k = 0;
  while (t > 0 && k < (int)sizeof(out->name) - 1)
    out->name[k++] = tmp[--t];
  out->name[k] = '\0';
  out->type = VFS_CHARDEVICE;
  out->size = 0;
  return 0;
}

struct vfs_ops aur_irq_dir_ops = {
    .readdir = aur_irq_dir_readdir,
};

struct aur_irq_ctx *aur_irq_get_ctx_pub(uint32_t irq) {
  return aur_irq_get_ctx(irq);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void aur_irq_init(void) {
  unsigned long f = spin_lock_irqsave(&g_aur_irq_init_lock);
  if (g_aur_irq_init_done) {
    spin_unlock_irqrestore(&g_aur_irq_init_lock, f);
    return;
  }
  for (int i = 0; i < IRQ_USER_MAX; i++) {
    spin_init(&g_irq_ctxs[i].lock);
    wait_queue_init(&g_irq_ctxs[i].wq);
    g_irq_ctxs[i].count = 0;
    g_irq_ctxs[i].registered = 0;
    g_irq_ctxs[i].refcount = 0;
    g_irq_ctxs[i].irq = (uint8_t)i;
  }
  g_aur_irq_init_done = 1;
  spin_unlock_irqrestore(&g_aur_irq_init_lock, f);
  LOG_INFO("[IRQ-USER] /dev/irq listo (16 legacy + %u MSI)",
           (unsigned)(MSI_VECTOR_END - MSI_VECTOR_BASE + 1));
}