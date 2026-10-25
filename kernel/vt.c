// kernel/vt.c
//
// /dev/tty0, /dev/tty1 (4:0, 4:1) con los ioctls de consola virtual de Linux
// (VT_*, KD*) que usan libseat (backend builtin), Weston, wlroots y X11 al
// entrar y salir del modo grafico. AuroraOS no tiene VTs reales: hay un unico
// VT (1) siempre activo y el compositor de Aurora sigue siendo el dueño de la
// pantalla; estos ioctls solo mantienen un estado coherente.
//
// Escrituras a /dev/ttyN se descartan; las lecturas no devuelven datos.

#include "devtmpfs.h"
#include "heap.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602
#define VT_GETSTATE 0x5603
#define VT_RELDISP 0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608
#define KDGETMODE 0x4B3B
#define KDSETMODE 0x4B3A
#define KDGKBTYPE 0x4B33
#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define KDGETLED 0x4B31
#define KDSETLED 0x4B32

#define KD_TEXT 0
#define KD_GRAPHICS 1
#define K_UNICODE 3
#define VT_AUTO 0

struct vt_mode_u {
  char mode;
  char waitv;
  int16_t relsig;
  int16_t acqsig;
  int16_t frsig;
};

struct vt_stat_u {
  uint16_t v_active;
  uint16_t v_signal;
  uint16_t v_state;
};

static spinlock_t g_vt_lock;
static int g_kd_mode = KD_TEXT;
static int g_kb_mode = K_UNICODE;
static struct vt_mode_u g_vt_mode; // VT_AUTO por defecto

static int64_t vt_read(vfs_node_t *node, uint64_t off, size_t size, void *buf) {
  (void)node;
  (void)off;
  (void)size;
  (void)buf;
  return 0;
}

static int64_t vt_write(vfs_node_t *node, uint64_t off, size_t size,
                        const void *buf) {
  (void)node;
  (void)off;
  (void)buf;
  return (int64_t)size;
}

static int put_u(uint64_t arg, const void *src, size_t n) {
  if (!arg || !access_ok((void *)arg, n) || copy_to_user((void *)arg, src, n) < 0)
    return -EFAULT;
  return 0;
}

static int get_u(uint64_t arg, void *dst, size_t n) {
  if (!arg || !access_ok((void *)arg, n) ||
      copy_from_user(dst, (void *)arg, n) < 0)
    return -EFAULT;
  return 0;
}

static int64_t vt_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  (void)node;
  switch (req) {
  case VT_OPENQRY: {
    int free_vt = 2; // el VT 1 es "la consola"
    return put_u(arg, &free_vt, sizeof(free_vt));
  }
  case VT_GETMODE: {
    unsigned long fl = spin_lock_irqsave(&g_vt_lock);
    struct vt_mode_u m = g_vt_mode;
    spin_unlock_irqrestore(&g_vt_lock, fl);
    return put_u(arg, &m, sizeof(m));
  }
  case VT_SETMODE: {
    struct vt_mode_u m;
    int rc = get_u(arg, &m, sizeof(m));
    if (rc)
      return rc;
    unsigned long fl = spin_lock_irqsave(&g_vt_lock);
    g_vt_mode = m;
    spin_unlock_irqrestore(&g_vt_lock, fl);
    return 0;
  }
  case VT_GETSTATE: {
    struct vt_stat_u s = {1, 0, 2}; // VT 1 activo (bit 1 = VT 1)
    return put_u(arg, &s, sizeof(s));
  }
  case VT_RELDISP:
  case VT_ACTIVATE:
  case VT_WAITACTIVE:
  case VT_DISALLOCATE:
    return 0; // el VT 1 ya es el activo
  case KDGETMODE: {
    unsigned long fl = spin_lock_irqsave(&g_vt_lock);
    int m = g_kd_mode;
    spin_unlock_irqrestore(&g_vt_lock, fl);
    return put_u(arg, &m, sizeof(m));
  }
  case KDSETMODE: {
    if (arg != KD_TEXT && arg != KD_GRAPHICS)
      return -EINVAL;
    unsigned long fl = spin_lock_irqsave(&g_vt_lock);
    g_kd_mode = (int)arg;
    spin_unlock_irqrestore(&g_vt_lock, fl);
    return 0;
  }
  case KDGKBTYPE: {
    char t = 2; // KB_101
    return put_u(arg, &t, 1);
  }
  case KDGKBMODE: {
    unsigned long fl = spin_lock_irqsave(&g_vt_lock);
    int m = g_kb_mode;
    spin_unlock_irqrestore(&g_vt_lock, fl);
    return put_u(arg, &m, sizeof(m));
  }
  case KDSKBMODE: {
    unsigned long fl = spin_lock_irqsave(&g_vt_lock);
    g_kb_mode = (int)arg;
    spin_unlock_irqrestore(&g_vt_lock, fl);
    return 0;
  }
  case KDGETLED: {
    char l = 0;
    return put_u(arg, &l, 1);
  }
  case KDSETLED:
    return 0;
  default:
    return -ENOTTY;
  }
}

static int vt_poll(vfs_node_t *node, short events) {
  (void)node;
  return (events & 4) ? 4 : 0;
}

static vfs_ops_t vt_ops = {
    .read = vt_read,
    .write = vt_write,
    .poll = vt_poll,
    .ioctl = vt_ioctl,
};

void vt_init(void) {
  spin_init(&g_vt_lock);
  memset(&g_vt_mode, 0, sizeof(g_vt_mode));
  devtmpfs_register("tty0", DEVT_MKDEV(4, 0), 0620, 0, 5, &vt_ops, NULL);
  devtmpfs_register("tty1", DEVT_MKDEV(4, 1), 0620, 0, 5, &vt_ops, NULL);
}
