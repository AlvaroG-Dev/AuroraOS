// kernel/msi_user.c
//
// [FASE 4.5b] MSI para drivers de usuario. Ver el contrato en aur.h.
//
// El .aur pide con AURMSI_ALLOC que se habilite MSI en su dispositivo; el
// kernel reserva un vector del pool 0x68..0x6F, programa la capability y
// deja el vector listo para open("/dev/irq/<vector>") (irq_user.c), que es
// quien registra el handler. El MSI es edge-triggered: no hay mask/EOI de
// IOAPIC de por medio.

#include "msi_user.h"
#include "apic.h"
#include "aur.h"
#include "aur_loader.h"
#include "klog.h"
#include "pci.h"
#include "process.h"
#include "spinlock.h"
#include "string.h"
#include "test.h"
#include "uaccess.h"

#define MSI_POOL_SIZE (AUR_MSI_VEC_LAST - AUR_MSI_VEC_FIRST + 1)

struct msi_slot {
  vfs_node_t *owner; // nodo /dev/msi que lo reservó; NULL = libre
  uint32_t pid;
  uint8_t bus, slot, func;
};

static struct msi_slot g_slots[MSI_POOL_SIZE];
static spinlock_t g_lock = {.locked = 0};

int aur_msi_is_user_vector(uint32_t vector) {
  return vector >= AUR_MSI_VEC_FIRST && vector <= AUR_MSI_VEC_LAST;
}

uint32_t aur_msi_vector_pid(uint32_t vector) {
  if (!aur_msi_is_user_vector(vector))
    return 0;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  struct msi_slot *s = &g_slots[vector - AUR_MSI_VEC_FIRST];
  uint32_t pid = s->owner ? s->pid : 0;
  spin_unlock_irqrestore(&g_lock, fl);
  return pid;
}

static int msi_open(vfs_node_t *node, int flags) {
  (void)node;
  (void)flags;
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;
  if (!(proc->aur.caps & AUR_CAP_MSI)) {
    LOG_WARN("[MSI-USER] pid=%u sin AUR_CAP_MSI (caps=0x%x)", proc->pid,
             proc->aur.caps);
    return -EPERM;
  }
  return 0;
}

// Reserva un hueco libre; devuelve su índice o -1. Con g_lock tomado.
static int msi_slot_reserve(vfs_node_t *owner, uint32_t pid, uint8_t bus,
                            uint8_t slot, uint8_t func) {
  for (int i = 0; i < MSI_POOL_SIZE; i++) {
    if (!g_slots[i].owner) {
      g_slots[i] =
          (struct msi_slot){.owner = owner, .pid = pid, .bus = bus,
                            .slot = slot, .func = func};
      return i;
    }
  }
  return -1;
}

// Comprueba que el dispositivo existe, está en el manifiesto del .aur y tiene
// MSI o MSI-X. 0 o -errno.
static int msi_check_device(process_t *proc, uint8_t bus, uint8_t slot,
                            uint8_t func) {
  pci_device_t dev;
  if (pci_get_device(bus, slot, func, &dev) != 0)
    return -ENODEV;

  // Solo el dispositivo que el manifiesto declara (igual que mmap de BARs).
  // Aquí exigimos además que declare alguno: sin entradas de bus no hay
  // "su dispositivo" que defender.
  if (proc->aur.bus_match_count <= 0 ||
      !aur_info_allows_pci(&proc->aur, dev.vendor_id, dev.device_id,
                           dev.class_id, dev.subclass_id)) {
    LOG_WARN("[MSI-USER] pid=%u '%s': %02x:%02x.%x no está en su manifiesto",
             proc->pid, proc->aur.name, bus, slot, func);
    return -EPERM;
  }

  if (!pci_has_msi_any(bus, slot, func)) {
    LOG_INFO("[MSI-USER] pid=%u: %02x:%02x.%x sin capability MSI ni MSI-X",
             proc->pid, bus, slot, func);
    return -ENODEV;
  }
  return 0;
}

static int64_t msi_alloc(vfs_node_t *node, uint64_t arg) {
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;

  struct aur_msi_alloc a;
  if (copy_from_user(&a, (void *)arg, sizeof(a)) < 0)
    return -EFAULT;

  int rc = msi_check_device(proc, a.bus, a.slot, a.func);
  if (rc < 0)
    return rc;

  unsigned long fl = spin_lock_irqsave(&g_lock);
  int idx = msi_slot_reserve(node, proc->pid, a.bus, a.slot, a.func);
  spin_unlock_irqrestore(&g_lock, fl);
  if (idx < 0)
    return -ENOSPC;

  uint8_t vec = (uint8_t)(AUR_MSI_VEC_FIRST + idx);
  if (pci_enable_msi_any(a.bus, a.slot, a.func, vec, lapic_get_bsp_id()) != 0) {
    fl = spin_lock_irqsave(&g_lock);
    g_slots[idx].owner = NULL;
    spin_unlock_irqrestore(&g_lock, fl);
    return -ENODEV;
  }

  a.vector = vec;
  if (copy_to_user((void *)arg, &a, sizeof(a)) < 0)
    return -EFAULT; // el vector se libera al cerrar el fd
  LOG_INFO("[MSI-USER] pid=%u: %02x:%02x.%x -> vector 0x%02x", proc->pid,
           a.bus, a.slot, a.func, vec);
  return 0;
}

// AURMSI_ALLOC_N: varios vectores a la vez vía MSI-X (uno por entrada de la
// tabla); sin MSI-X, un único vector por MSI.
static int64_t msi_alloc_n(vfs_node_t *node, uint64_t arg) {
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;

  struct aur_msi_alloc_n a;
  if (copy_from_user(&a, (void *)arg, sizeof(a)) < 0)
    return -EFAULT;
  if (a.count < 1 || a.count > AUR_MSI_MAX_VECS)
    return -EINVAL;

  int rc = msi_check_device(proc, a.bus, a.slot, a.func);
  if (rc < 0)
    return rc;

  int tsize = pci_msix_table_size(a.bus, a.slot, a.func);
  int want = a.count;
  if (tsize == 0)
    want = 1; // solo MSI: un vector
  else if (want > tsize)
    want = tsize;

  // Reservar hasta `want` huecos; con menos libres se concede lo que haya.
  int idx[AUR_MSI_MAX_VECS];
  int got = 0;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  while (got < want) {
    int i = msi_slot_reserve(node, proc->pid, a.bus, a.slot, a.func);
    if (i < 0)
      break;
    idx[got++] = i;
  }
  spin_unlock_irqrestore(&g_lock, fl);
  if (got == 0)
    return -ENOSPC;

  uint8_t vec[AUR_MSI_MAX_VECS];
  for (int i = 0; i < got; i++)
    vec[i] = (uint8_t)(AUR_MSI_VEC_FIRST + idx[i]);

  int err = tsize ? pci_enable_msix(a.bus, a.slot, a.func, vec, got,
                                    lapic_get_bsp_id())
                  : pci_enable_msi(a.bus, a.slot, a.func, vec[0],
                                   lapic_get_bsp_id());
  if (err != 0) {
    fl = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < got; i++)
      g_slots[idx[i]].owner = NULL;
    spin_unlock_irqrestore(&g_lock, fl);
    return -ENODEV;
  }

  a.count = (uint8_t)got;
  for (int i = 0; i < AUR_MSI_MAX_VECS; i++)
    a.vectors[i] = i < got ? vec[i] : 0;
  if (copy_to_user((void *)arg, &a, sizeof(a)) < 0)
    return -EFAULT; // los vectores se liberan al cerrar el fd
  LOG_INFO("[MSI-USER] pid=%u: %02x:%02x.%x -> %d vector(es), primero 0x%02x%s",
           proc->pid, a.bus, a.slot, a.func, got, vec[0],
           tsize ? " (MSI-X)" : " (MSI)");
  return 0;
}

static int64_t msi_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  switch (req) {
  case AURMSI_ALLOC:
    return msi_alloc(node, arg);
  case AURMSI_ALLOC_N:
    return msi_alloc_n(node, arg);
  default:
    return -ENOTTY;
  }
}

// El VFS llama a close() al destruir cualquier nodo; solo libera los
// vectores que reservó este nodo.
static int msi_close(vfs_node_t *node) {
  for (int i = 0; i < MSI_POOL_SIZE; i++) {
    unsigned long fl = spin_lock_irqsave(&g_lock);
    struct msi_slot s = g_slots[i];
    int mine = (s.owner == node);
    if (mine)
      g_slots[i].owner = NULL;
    spin_unlock_irqrestore(&g_lock, fl);
    if (mine) {
      pci_disable_msi(s.bus, s.slot, s.func);
      LOG_INFO("[MSI-USER] vector 0x%02x liberado (%02x:%02x.%x)",
               AUR_MSI_VEC_FIRST + i, s.bus, s.slot, s.func);
    }
  }
  return 0;
}

vfs_ops_t msi_dev_ops = {
    .open = msi_open,
    .close = msi_close,
    .ioctl = msi_ioctl,
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void test_msi_pool(void) {
  TEST_ASSERT(!aur_msi_is_user_vector(0x67) && aur_msi_is_user_vector(0x68) &&
                  aur_msi_is_user_vector(0x6F) && !aur_msi_is_user_vector(0x70),
              "rango del pool de usuario");
  TEST_ASSERT_EQ(aur_msi_vector_pid(0x60), 0);

  vfs_node_t fake;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  int free_before = 0;
  for (int i = 0; i < MSI_POOL_SIZE; i++)
    free_before += g_slots[i].owner == NULL;
  int a = msi_slot_reserve(&fake, 77, 0, 1, 0);
  int b = msi_slot_reserve(&fake, 77, 0, 2, 0);
  spin_unlock_irqrestore(&g_lock, fl);
  if (free_before < 2) {
    test_skip("pool MSI en uso");
  } else {
    TEST_ASSERT(a >= 0 && b >= 0 && a != b, "reserva dos vectores distintos");
    TEST_ASSERT_EQ(aur_msi_vector_pid((uint32_t)(AUR_MSI_VEC_FIRST + a)), 77);
  }
  // Liberar lo reservado sin tocar el hardware (no hay MSI real aquí).
  fl = spin_lock_irqsave(&g_lock);
  for (int i = 0; i < MSI_POOL_SIZE; i++)
    if (g_slots[i].owner == &fake)
      g_slots[i].owner = NULL;
  spin_unlock_irqrestore(&g_lock, fl);
}
REGISTER_TEST("msi_user: pool de vectores", test_msi_pool);
