// kernel/input_proxy.c
//
// [FASE 5.5] Puente entre el subsistema de entrada y un driver PS/2 en Ring 3.
//
// Un .aur con AUR_CAP_INPUT_DEVICE abre /dev/inputproxy. El kernel le presta
// dos puertos de E/S (0x60 datos, 0x64 estado/comando del i8042) y recibe los
// bytes crudos que el driver lee del controlador. Ver el contrato en aur.h.
//
//   ps2.aur ──INPUTP_PORT_IN/OUT──► inb/outb (lista blanca 0x60, 0x64)
//   ps2.aur ──INPUTP_FEED─────────► ps2_feed ► ps2_thread ► input_push / TTY
//
// Un único propietario: si dos procesos pudieran alimentar la cola del PS/2
// se rompería su invariante de productor único.

#include "input_proxy.h"
#include "aur.h"
#include "klog.h"
#include "process.h"
#include "ps2.h"
#include "spinlock.h"
#include "string.h"
#include "test.h"
#include "uaccess.h"

static spinlock_t g_lock = {.locked = 0};
static vfs_node_t *g_owner; // nodo VFS que hizo el open con éxito

// Puertos que ps2.aur puede tocar: solo el i8042.
static int inputp_port_allowed(uint16_t port) {
  return port == 0x60 || port == 0x64;
}

static int inputp_feed_valid(const struct aur_input_feed *f) {
  return (f->kind == AUR_INPUT_KBD || f->kind == AUR_INPUT_MOUSE) &&
         f->len >= 1 && f->len <= AUR_INPUT_MAX_FEED;
}

static int inputp_open(vfs_node_t *node, int flags) {
  (void)flags;
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;
  if (!(proc->aur.caps & AUR_CAP_INPUT_DEVICE)) {
    LOG_WARN("[INPUTP] pid=%u sin AUR_CAP_INPUT_DEVICE (caps=0x%x)", proc->pid,
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
  LOG_INFO("[INPUTP] driver de entrada pid=%u conectado", proc->pid);
  return 0;
}

static int inputp_close(vfs_node_t *node) {
  // El VFS llama a close() al destruir CUALQUIER nodo (stat, open fallido
  // por EBUSY...). Solo el nodo que hizo el open con éxito desconecta.
  unsigned long fl = spin_lock_irqsave(&g_lock);
  int mine = (node == g_owner);
  if (mine)
    g_owner = NULL;
  spin_unlock_irqrestore(&g_lock, fl);
  if (mine)
    LOG_WARN("[INPUTP] driver de entrada desconectado");
  return 0;
}

static int64_t inputp_ioctl(vfs_node_t *node, unsigned long req,
                            uint64_t arg) {
  // Solo el propietario (el que hizo el open con éxito) opera.
  unsigned long fl = spin_lock_irqsave(&g_lock);
  int mine = (node == g_owner);
  spin_unlock_irqrestore(&g_lock, fl);
  if (!mine)
    return -EPERM;

  switch (req) {
  case INPUTP_PORT_IN:
  case INPUTP_PORT_OUT: {
    struct aur_input_port p;
    if (copy_from_user(&p, (void *)arg, sizeof(p)) < 0)
      return -EFAULT;
    if (!inputp_port_allowed(p.port))
      return -EPERM;
    if (req == INPUTP_PORT_OUT) {
      __asm__ volatile("outb %0, %1" : : "a"(p.value), "dN"(p.port));
      return 0;
    }
    __asm__ volatile("inb %1, %0" : "=a"(p.value) : "dN"(p.port));
    if (copy_to_user((void *)arg, &p, sizeof(p)) < 0)
      return -EFAULT;
    return 0;
  }
  case INPUTP_FEED: {
    struct aur_input_feed f;
    if (copy_from_user(&f, (void *)arg, sizeof(f)) < 0)
      return -EFAULT;
    if (!inputp_feed_valid(&f))
      return -EINVAL;
    ps2_feed(f.kind == AUR_INPUT_MOUSE, f.data, f.len);
    return 0;
  }
  case INPUTP_MOUSE_WHEEL:
    ps2_set_mouse_wheel(arg != 0);
    return 0;
  default:
    return -ENOTTY;
  }
}

vfs_ops_t inputp_dev_ops = {
    .open = inputp_open,
    .close = inputp_close,
    .ioctl = inputp_ioctl,
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void test_inputp_validation(void) {
  TEST_ASSERT(inputp_port_allowed(0x60) && inputp_port_allowed(0x64),
              "puertos del i8042");
  TEST_ASSERT(!inputp_port_allowed(0x61) && !inputp_port_allowed(0x80) &&
                  !inputp_port_allowed(0xCF8) && !inputp_port_allowed(0),
              "resto de puertos denegados");

  struct aur_input_feed f;
  memset(&f, 0, sizeof(f));
  f.kind = AUR_INPUT_KBD;
  f.len = 1;
  TEST_ASSERT(inputp_feed_valid(&f), "feed válido");
  f.len = 0;
  TEST_ASSERT(!inputp_feed_valid(&f), "len 0");
  f.len = AUR_INPUT_MAX_FEED + 1;
  TEST_ASSERT(!inputp_feed_valid(&f), "len excesivo");
  f.len = AUR_INPUT_MAX_FEED;
  f.kind = 2;
  TEST_ASSERT(!inputp_feed_valid(&f), "kind inválido");
}
REGISTER_TEST("inputproxy: validación de puertos y feed", test_inputp_validation);
