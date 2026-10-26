// kernel/gpu_proxy.c
//
// [FASE 6.1] Puente entre el compositor/DRM y un driver de pantalla en Ring 3
// (virtio_gpu.aur, bochs.aur...). Contrato en kernel/aur.h (GPUP_*).
//
//   compositor.present_rect ──gpup_notify_dirty──► rect sucio acumulado
//   driver (.aur)           ──GPUP_WAIT_PRESENT──► (rect) ──► TRANSFER+FLUSH
//
// El tráfico por frame es un solo rectángulo fusionado: el compositor no
// espera al driver y un driver lento nunca bloquea el dibujado.

#include "gpu_proxy.h"
#include "aur.h"
#include "klog.h"
#include "process.h"
#include "spinlock.h"
#include "string.h"
#include "test.h"
#include "uaccess.h"
#include "completion.h"
#include "pci.h"
#include "pmm.h"
#include "wait.h"
#include <stdbool.h>

// main.c: adopta un framebuffer en RAM y arranca la pantalla con el.
extern int fb_adopt_ram(uint64_t phys, uint64_t size, uint32_t w, uint32_t h,
                        uint32_t pitch);
static completion_t g_display_done;

static spinlock_t g_lock = {.locked = 0};
static vfs_node_t *g_owner;
static wait_queue_t g_wq;
static int g_init_done;

static struct aur_gpu_info g_fb;
static struct aur_gpu_reg g_reg;
static int g_registered;
static int g_needs_present;

// Rectángulo sucio acumulado [x0,x1)x[y0,y1); vacío si x1 <= x0.
static int g_dx0, g_dy0, g_dx1, g_dy1;
static uint32_t g_seq;

static void gpup_clear_dirty(void) { g_dx0 = g_dy0 = g_dx1 = g_dy1 = 0; }

// Fusiona (x,y,w,h) en el rectángulo sucio, limitado al framebuffer.
static void gpup_union(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  int x1 = x + w, y1 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (g_fb.width && x1 > (int)g_fb.width) x1 = (int)g_fb.width;
  if (g_fb.height && y1 > (int)g_fb.height) y1 = (int)g_fb.height;
  if (x1 <= x || y1 <= y)
    return;
  if (g_dx1 <= g_dx0) {
    g_dx0 = x; g_dy0 = y; g_dx1 = x1; g_dy1 = y1;
    return;
  }
  if (x < g_dx0) g_dx0 = x;
  if (y < g_dy0) g_dy0 = y;
  if (x1 > g_dx1) g_dx1 = x1;
  if (y1 > g_dy1) g_dy1 = y1;
}

int gpup_wait_display(uint32_t timeout_ms) {
  if (g_fb.fb_phys)
    return 1;
  return wait_for_completion_timeout(&g_display_done, timeout_ms) != 0;
}

int gpup_display_expected(void) {
  pci_device_t d;
  return pci_find_device(0x03, 0x00, &d) == 0 ||
         pci_find_device(0x03, 0x80, &d) == 0;
}

void gpup_set_framebuffer(uint64_t phys, uint64_t size, uint32_t w, uint32_t h,
                          uint32_t pitch, uint32_t bpp) {
  g_fb.fb_phys = phys;
  g_fb.fb_size = size;
  g_fb.width = w;
  g_fb.height = h;
  g_fb.pitch = pitch;
  g_fb.bpp = bpp;
}

void gpup_notify_dirty(int x, int y, int w, int h) {
  if (!__atomic_load_n(&g_needs_present, __ATOMIC_ACQUIRE))
    return;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (g_needs_present) {
    gpup_union(x, y, w, h);
    g_seq++;
  }
  spin_unlock_irqrestore(&g_lock, fl);
  wake_up_all(&g_wq);
}

const struct aur_gpu_reg *gpup_adapter(void) {
  return g_registered ? &g_reg : NULL;
}

static bool gpup_has_work(void *arg) {
  (void)arg;
  return g_dx1 > g_dx0 || !g_needs_present /* desconectado: salir */;
}

static int gpup_open(vfs_node_t *node, int flags) {
  (void)flags;
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;
  if (!(proc->aur.caps & AUR_CAP_GPU)) {
    LOG_WARN("[GPUP] pid=%u sin AUR_CAP_GPU (caps=0x%x)", proc->pid,
             proc->aur.caps);
    return -EPERM;
  }
  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (g_owner) {
    spin_unlock_irqrestore(&g_lock, fl);
    return -EBUSY;
  }
  g_owner = node;
  spin_unlock_irqrestore(&g_lock, fl);
  LOG_INFO("[GPUP] driver de pantalla pid=%u conectado", proc->pid);
  return 0;
}

static int gpup_close(vfs_node_t *node) {
  unsigned long fl = spin_lock_irqsave(&g_lock);
  int mine = (node == g_owner);
  if (mine) {
    g_owner = NULL;
    g_registered = 0;
    g_needs_present = 0;
    gpup_clear_dirty();
    memset(&g_reg, 0, sizeof(g_reg));
  }
  spin_unlock_irqrestore(&g_lock, fl);
  if (mine) {
    wake_up_all(&g_wq);
    LOG_WARN("[GPUP] driver de pantalla desconectado");
  }
  return 0;
}

// Valida lo que declara un driver (puro: se prueba sin hardware).
static int gpup_reg_valid(const struct aur_gpu_reg *r) {
  if (r->n_modes > AUR_GPU_MAX_MODES)
    return 0;
  if (r->edid_len != 0 && r->edid_len != 128)
    return 0;
  for (uint32_t i = 0; i < r->n_modes; i++)
    if (r->modes[i].width < 64 || r->modes[i].height < 64 ||
        r->modes[i].width > 16384 || r->modes[i].height > 16384)
      return 0;
  return 1;
}

static int64_t gpup_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  unsigned long fl = spin_lock_irqsave(&g_lock);
  int mine = (node == g_owner);
  spin_unlock_irqrestore(&g_lock, fl);
  if (!mine)
    return -EPERM;

  switch (req) {
  case GPUP_INFO:
    if (!g_fb.fb_phys)
      return -ENODEV;
    if (copy_to_user((void *)arg, &g_fb, sizeof(g_fb)) < 0)
      return -EFAULT;
    return 0;
  case GPUP_ALLOC_FB: {
    struct aur_gpu_info want;
    if (copy_from_user(&want, (void *)arg, sizeof(want)) < 0)
      return -EFAULT;
    if (!g_fb.fb_phys) {
      uint32_t w = want.width & ~1u, h = want.height;
      if (w < 640 || h < 480 || w > 4096 || h > 4096)
        return -EINVAL;
      uint64_t size = (uint64_t)w * h * 4;
      uint64_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
      uint64_t phys = pmm_alloc_pages(npages);
      if (!phys)
        return -ENOMEM;
      memset(phys_to_virt(phys), 0, npages * PAGE_SIZE);
      int rc = fb_adopt_ram(phys, npages * PAGE_SIZE, w, h, w * 4);
      if (rc != 0) {
        pmm_free_pages(phys, npages);
        return rc;
      }
      LOG_INFO("[GPUP] framebuffer en RAM %ux%u en 0x%lx pedido por el driver",
               w, h, (unsigned long)phys);
      complete_all(&g_display_done);
    }
    if (copy_to_user((void *)arg, &g_fb, sizeof(g_fb)) < 0)
      return -EFAULT;
    return 0;
  }
  case GPUP_REGISTER: {
    static struct aur_gpu_reg tmp; // 304 B: no en la pila del kernel
    if (copy_from_user(&tmp, (void *)arg, sizeof(tmp)) < 0)
      return -EFAULT;
    tmp.name[sizeof(tmp.name) - 1] = 0;
    if (!gpup_reg_valid(&tmp))
      return -EINVAL;
    unsigned long f = spin_lock_irqsave(&g_lock);
    memcpy(&g_reg, &tmp, sizeof(g_reg));
    g_registered = 1;
    gpup_clear_dirty();
    // Primera presentación completa: el recurso del driver aún está vacío.
    g_needs_present = (tmp.flags & AUR_GPU_NEEDS_PRESENT) != 0;
    if (g_needs_present)
      gpup_union(0, 0, (int)g_fb.width, (int)g_fb.height);
    spin_unlock_irqrestore(&g_lock, f);
    LOG_INFO("[GPUP] adaptador '%s': %u modos, edid=%u, present=%d", tmp.name,
             tmp.n_modes, tmp.edid_len, g_needs_present);
    wake_up_all(&g_wq);
    return 0;
  }
  case GPUP_WAIT_PRESENT: {
    struct aur_gpu_present p;
    if (copy_from_user(&p, (void *)arg, sizeof(p)) < 0)
      return -EFAULT;
    for (;;) {
      unsigned long f = spin_lock_irqsave(&g_lock);
      if (!g_registered || !g_needs_present) {
        spin_unlock_irqrestore(&g_lock, f);
        return -ENODEV;
      }
      if (g_dx1 > g_dx0) {
        p.x = (uint32_t)g_dx0;
        p.y = (uint32_t)g_dy0;
        p.w = (uint32_t)(g_dx1 - g_dx0);
        p.h = (uint32_t)(g_dy1 - g_dy0);
        p.seq = g_seq;
        gpup_clear_dirty();
        spin_unlock_irqrestore(&g_lock, f);
        break;
      }
      spin_unlock_irqrestore(&g_lock, f);
      if (p.flags & AUR_GPU_NOWAIT)
        return -EAGAIN;
      int rc = wait_event_interruptible(&g_wq, gpup_has_work, NULL);
      if (rc < 0)
        return rc;
    }
    if (copy_to_user((void *)arg, &p, sizeof(p)) < 0)
      return -EFAULT;
    return 0;
  }
  default:
    return -ENOTTY;
  }
}

static int gpup_poll(vfs_node_t *node, short events) {
  (void)node;
  return ((events & 1) && g_dx1 > g_dx0) ? 1 : 0;
}

static wait_queue_t *gpup_poll_wq(vfs_node_t *node) {
  (void)node;
  return &g_wq;
}

vfs_ops_t gpup_dev_ops = {
    .open = gpup_open,
    .close = gpup_close,
    .ioctl = gpup_ioctl,
    .poll = gpup_poll,
    .poll_wq = gpup_poll_wq,
};

void gpup_init(void) {
  if (__atomic_exchange_n(&g_init_done, 1, __ATOMIC_ACQ_REL))
    return;
  wait_queue_init(&g_wq);
  completion_init(&g_display_done);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void test_gpup_validation(void) {
  struct aur_gpu_reg r;
  memset(&r, 0, sizeof(r));
  TEST_ASSERT(gpup_reg_valid(&r), "registro vacío válido");
  r.n_modes = 1;
  r.modes[0].width = 1280;
  r.modes[0].height = 720;
  TEST_ASSERT(gpup_reg_valid(&r), "un modo 1280x720");
  r.modes[0].width = 32;
  TEST_ASSERT(!gpup_reg_valid(&r), "modo demasiado pequeño");
  r.modes[0].width = 1280;
  r.n_modes = AUR_GPU_MAX_MODES + 1;
  TEST_ASSERT(!gpup_reg_valid(&r), "demasiados modos");
  r.n_modes = 0;
  r.edid_len = 64;
  TEST_ASSERT(!gpup_reg_valid(&r), "EDID de longitud inválida");
  r.edid_len = 128;
  TEST_ASSERT(gpup_reg_valid(&r), "EDID de 128 bytes");
}

static void test_gpup_union(void) {
  gpup_set_framebuffer(0x1000, 1920 * 1080 * 4, 1920, 1080, 1920 * 4, 32);
  gpup_clear_dirty();
  gpup_union(10, 20, 100, 50);
  TEST_ASSERT(g_dx0 == 10 && g_dy0 == 20 && g_dx1 == 110 && g_dy1 == 70,
              "primer rectángulo");
  gpup_union(200, 5, 10, 10);
  TEST_ASSERT(g_dx0 == 10 && g_dy0 == 5 && g_dx1 == 210 && g_dy1 == 70,
              "fusión en el rectángulo envolvente");
  gpup_clear_dirty();
  gpup_union(1900, 1070, 100, 100);
  TEST_ASSERT(g_dx1 == 1920 && g_dy1 == 1080, "recorte al framebuffer");
  gpup_clear_dirty();
  gpup_union(-50, -50, 20, 20);
  TEST_ASSERT(g_dx1 <= g_dx0, "fuera de pantalla: nada");
  gpup_clear_dirty();
  gpup_set_framebuffer(0, 0, 0, 0, 0, 0);
}

REGISTER_TEST("gpuproxy: validación del registro", test_gpup_validation);
REGISTER_TEST("gpuproxy: rectángulo sucio fusionado", test_gpup_union);
