// kernel/aur_bus.c
#include "aur_bus.h"
#include "aur_loader.h"
#include "elf.h"
#include "klog.h"
#include "kthread.h"
#include "pci.h"
#include "process.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "test.h"
#include "uaccess.h"
#include "vfs.h"
#include "wait.h"

// Wrapper para leer un ELF desde el VFS (necesario para el parser)
static int64_t aur_read_vfs(void *ctx, uint64_t offset, size_t size,
                            void *buf) {
  vfs_node_t *node = (vfs_node_t *)ctx;
  if (!node || !node->ops || !node->ops->read)
    return -1;
  return node->ops->read(node, offset, size, buf);
}

// Comprueba si un dispositivo físico coincide con los IDs de un manifiesto .aur
static int aur_bus_match_manifest(struct device *dev,
                                  const struct aur_module_info *info) {
  if (!dev || !info || info->bus_match_count == 0)
    return 0;

  for (int i = 0; i < info->bus_match_count; i++) {
    const aur_bus_match_t *m = &info->bus_match[i];

    // Match para bus PCI
    if (m->bus == AUR_BUS_PCI && strcmp(dev->bus->name, "pci") == 0) {
      pci_device_t *pdev = (pci_device_t *)dev->priv;
      if (!pdev)
        continue;

      // Si el manifiesto especifica vendor/device y no coinciden, saltar
      if (m->vendor != 0 && m->vendor != pdev->vendor_id)
        continue;
      if (m->device != 0 && m->device != pdev->device_id)
        continue;

      // Si se especifica class_mask, comprobar clase y subclase
      if (m->class_mask != 0) {
        if ((pdev->class_id & m->class_mask) != (m->class_id & m->class_mask))
          continue;
        if ((pdev->subclass_id & m->class_mask) !=
            (m->subclass_id & m->class_mask))
          continue;
      }

      return 1; // ¡Match!
    }
  }
  return 0;
}

// Manifiestos de /usr/lib/aur/*.aur, leídos UNA vez. Antes cada probe de cada
// dispositivo PCI sin driver volvía a abrir y parsear todos los .aur (y
// escribía ~3 líneas de log por fichero y dispositivo): en máquinas con log
// lento eso eran varios segundos de arranque.
#define AUR_BUS_MAX_MODULES 16

static struct {
  char path[VFS_PATH_MAX];
  struct aur_module_info info;
} g_aur_mods[AUR_BUS_MAX_MODULES];
static int g_aur_nmods;
static volatile int g_aur_scan_state; // 0 = pendiente, 1 = escaneando, 2 = hecho

// Devuelve 0 si el catálogo está listo, -ENODEV si /usr/lib/aur no existe
// todavía (se reintentará en el próximo probe).
static int aur_bus_scan_once(void) {
  if (__atomic_load_n(&g_aur_scan_state, __ATOMIC_ACQUIRE) == 2)
    return 0;

  int expected = 0;
  if (!__atomic_compare_exchange_n(&g_aur_scan_state, &expected, 1, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    // Otro hilo está escaneando: esperar.
    while (__atomic_load_n(&g_aur_scan_state, __ATOMIC_ACQUIRE) == 1)
      sched_yield();
    return __atomic_load_n(&g_aur_scan_state, __ATOMIC_ACQUIRE) == 2 ? 0
                                                                      : -ENODEV;
  }

  vfs_node_t *dir = vfs_lookup("/usr/lib/aur");
  if (!dir || !(dir->flags & VFS_DIRECTORY)) {
    if (dir)
      vfs_node_free(dir);
    LOG_WARN("[AUR-BUS] /usr/lib/aur no existe o no es un directorio");
    __atomic_store_n(&g_aur_scan_state, 0, __ATOMIC_RELEASE);
    return -ENODEV;
  }

  vfs_dirent_t ent;
  for (uint64_t i = 0; i < 1024 && g_aur_nmods < AUR_BUS_MAX_MODULES; i++) {
    if (vfs_readdir_node(dir, i, &ent) != 0 || ent.name[0] == '\0')
      break;
    size_t nlen = strlen(ent.name);
    if (nlen < 5 || strcmp(ent.name + nlen - 4, ".aur") != 0)
      continue;

    char full_path[VFS_PATH_MAX] = "/usr/lib/aur/";
    size_t base_len = strlen(full_path);
    if (base_len + nlen + 1 > sizeof(full_path))
      continue;
    memcpy(full_path + base_len, ent.name, nlen + 1);

    vfs_node_t *node = vfs_lookup(full_path);
    if (!node)
      continue;
    int slot = g_aur_nmods;
    memset(&g_aur_mods[slot].info, 0, sizeof(g_aur_mods[slot].info));
    int rc = aur_loader_parse_streaming(aur_read_vfs, node, node->size,
                                        &g_aur_mods[slot].info);
    vfs_node_free(node);
    if (rc == 0) {
      memcpy(g_aur_mods[slot].path, full_path, sizeof(full_path));
      g_aur_nmods++;
      LOG_INFO("[AUR-BUS] módulo %s (caps=0x%x, %d match)", full_path,
               g_aur_mods[slot].info.caps,
               (int)g_aur_mods[slot].info.bus_match_count);
    }
  }
  vfs_node_free(dir);

  __atomic_store_n(&g_aur_scan_state, 2, __ATOMIC_RELEASE);
  return 0;
}

// ---------------------------------------------------------------------------
// [FASE 6.3] Supervisor de drivers .aur
//
// Los .aur que lanza el bus quedan huérfanos (ppid 0). Un kthread los vigila:
// si uno termina con código != 0 (fallo, señal) lo recoge y lo relanza con
// backoff exponencial. Tras AUR_SUP_MAX_RESTARTS reinicios seguidos sin
// aguantar AUR_SUP_STABLE_MS vivo, se da por perdido. Una salida con código 0
// se considera voluntaria y no se reinicia.
//
// Los proxies (blkproxy, net0, usbproxy) ya toleran la reconexión: al cerrarse
// el fd abortan lo pendiente y el siguiente open() vuelve a empezar.
// ---------------------------------------------------------------------------
#define AUR_SUP_MAX 8
#define AUR_SUP_MAX_RESTARTS 5
#define AUR_SUP_PERIOD_MS 500u
#define AUR_SUP_BACKOFF_MS 500u  // se duplica con cada reinicio seguido
#define AUR_SUP_STABLE_MS 30000u // vivo tanto tiempo => se resetea el contador

enum { SUP_FREE = 0, SUP_RUNNING, SUP_BACKOFF, SUP_GIVEN_UP };

struct aur_sup {
  int state;
  char path[VFS_PATH_MAX];
  uint32_t pid;
  int restarts;
  uint64_t started;  // tick del último lanzamiento
  uint64_t retry_at; // tick de relanzamiento (SUP_BACKOFF)
};

static struct aur_sup g_sup[AUR_SUP_MAX];
static spinlock_t g_sup_lock = {.locked = 0};
static int g_sup_thread_started;
static wait_queue_t g_sup_wq;

enum sup_action { SUP_NONE, SUP_RESTART_NOW };

// Un paso de la máquina de estados para una entrada, dado el resultado de
// recoger su proceso (`reap`: 0 = acaba de morir con `code`, -EBUSY = vivo,
// otro = desaparecido). Separado del kthread para poder probarlo.
// Devuelve SUP_RESTART_NOW si hay que relanzar ya. Llamar con g_sup_lock.
static enum sup_action aur_sup_step(struct aur_sup *s, uint64_t now, int reap,
                                    int code) {
  if (s->state == SUP_RUNNING) {
    if (reap == -EBUSY) {
      if (s->restarts && now - s->started > AUR_SUP_STABLE_MS)
        s->restarts = 0;
      return SUP_NONE;
    }
    if (reap == 0 && code == 0) {
      LOG_INFO("[AUR-SUP] %s (pid %u) salió con código 0, no se reinicia",
               s->path, s->pid);
      s->state = SUP_FREE;
      return SUP_NONE;
    }
    if (now - s->started > AUR_SUP_STABLE_MS)
      s->restarts = 0;
    if (s->restarts >= AUR_SUP_MAX_RESTARTS) {
      LOG_ERR("[AUR-SUP] %s murió (código %d) %d veces seguidas: abandonado",
              s->path, code, s->restarts);
      s->state = SUP_GIVEN_UP;
      return SUP_NONE;
    }
    uint64_t wait = (uint64_t)AUR_SUP_BACKOFF_MS << s->restarts;
    LOG_WARN("[AUR-SUP] %s (pid %u) murió con código %d; reinicio %d/%d en "
             "%lu ms",
             s->path, s->pid, code, s->restarts + 1, AUR_SUP_MAX_RESTARTS,
             (unsigned long)wait);
    s->retry_at = now + wait;
    s->state = SUP_BACKOFF;
    return SUP_NONE;
  }
  if (s->state == SUP_BACKOFF && now >= s->retry_at)
    return SUP_RESTART_NOW;
  return SUP_NONE;
}

static bool aur_sup_never(void *arg) {
  (void)arg;
  return false;
}

static int aur_sup_thread(void *arg) {
  (void)arg;
  for (;;) {
    for (int i = 0; i < AUR_SUP_MAX; i++) {
      struct aur_sup *s = &g_sup[i];
      uint64_t now = sched_get_ticks();

      unsigned long fl = spin_lock_irqsave(&g_sup_lock);
      int state = s->state;
      uint32_t pid = s->pid;
      spin_unlock_irqrestore(&g_sup_lock, fl);
      if (state != SUP_RUNNING && state != SUP_BACKOFF)
        continue;

      int code = 0;
      int reap = -EBUSY;
      if (state == SUP_RUNNING)
        reap = process_reap_orphan(pid, &code);

      fl = spin_lock_irqsave(&g_sup_lock);
      enum sup_action act = aur_sup_step(s, now, reap, code);
      char path[VFS_PATH_MAX];
      if (act == SUP_RESTART_NOW)
        memcpy(path, s->path, sizeof(path));
      spin_unlock_irqrestore(&g_sup_lock, fl);

      if (act != SUP_RESTART_NOW)
        continue;
      process_t *p = process_load(path);
      fl = spin_lock_irqsave(&g_sup_lock);
      s->restarts++;
      s->started = sched_get_ticks();
      if (p) {
        s->pid = p->pid;
        s->state = SUP_RUNNING;
        LOG_INFO("[AUR-SUP] %s relanzado (pid %u)", path, p->pid);
      } else {
        s->retry_at =
            s->started + ((uint64_t)AUR_SUP_BACKOFF_MS << s->restarts);
        s->state =
            s->restarts >= AUR_SUP_MAX_RESTARTS ? SUP_GIVEN_UP : SUP_BACKOFF;
        LOG_ERR("[AUR-SUP] fallo al relanzar %s", path);
      }
      spin_unlock_irqrestore(&g_sup_lock, fl);
    }
    wait_event_timeout(&g_sup_wq, aur_sup_never, NULL, AUR_SUP_PERIOD_MS);
  }
  return 0;
}

static void aur_sup_register(const char *path, uint32_t pid) {
  unsigned long fl = spin_lock_irqsave(&g_sup_lock);
  struct aur_sup *s = NULL;
  for (int i = 0; i < AUR_SUP_MAX; i++) {
    if (g_sup[i].state == SUP_FREE) {
      s = &g_sup[i];
      break;
    }
  }
  int start = 0;
  if (s) {
    memset(s, 0, sizeof(*s));
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->pid = pid;
    s->started = sched_get_ticks();
    s->state = SUP_RUNNING;
    start = !g_sup_thread_started;
    g_sup_thread_started = 1;
  }
  spin_unlock_irqrestore(&g_sup_lock, fl);
  if (!s) {
    LOG_WARN("[AUR-SUP] tabla llena: %s no será supervisado", path);
    return;
  }
  if (start) {
    wait_queue_init(&g_sup_wq);
    if (!kthread_run(aur_sup_thread, NULL, "aur-sup")) {
      LOG_ERR("[AUR-SUP] no se pudo crear el supervisor");
      fl = spin_lock_irqsave(&g_sup_lock);
      g_sup_thread_started = 0;
      spin_unlock_irqrestore(&g_sup_lock, fl);
    }
  }
}

// Función probe: se llama cuando el bus decide probar este driver contra un
// dispositivo. Solo loguea (INFO) cuando algún .aur coincide; "ningún .aur"
// es el caso normal de casi todos los dispositivos PCI.
static int aur_bus_probe(struct device *dev) {
  if (aur_bus_scan_once() != 0)
    return -ENODEV;

  for (int i = 0; i < g_aur_nmods; i++) {
    if (!aur_bus_match_manifest(dev, &g_aur_mods[i].info))
      continue;

    LOG_INFO("[AUR-BUS] %s -> %s", dev->name, g_aur_mods[i].path);
    // El .aur no puede escribir el config space: habilitar aquí el bus
    // mastering y MEM/IO para que pueda hacer DMA y usar los BARs.
    pci_device_t *pdev = (pci_device_t *)dev->priv;
    if (pdev) {
      pci_enable_bus_mastering(pdev);
      pci_enable_io_mem(pdev);
    }
    process_t *p = process_load(g_aur_mods[i].path);
    if (p) {
      aur_sup_register(g_aur_mods[i].path, p->pid);
      return 0;
    }
    LOG_ERR("[AUR-BUS] Fallo al lanzar %s", g_aur_mods[i].path);
  }

  LOG_DEBUG("[AUR-BUS] ningún .aur reclama %s", dev->name);
  return -ENODEV;
}

// Tabla de IDs. Al usar PCI_ANY_ID, este driver acepta probar CUALQUIER
// dispositivo PCI que haya sido rechazado por los drivers del kernel.
static const pci_device_id_t aur_pci_id_table[] = {
    {
        .vendor_id = PCI_ANY_ID,
        .device_id = PCI_ANY_ID,
        .subvendor = PCI_ANY_ID, // <-- FALTABA
        .subdevice = PCI_ANY_ID  // <-- FALTABA
    },
    {0}};

struct driver aur_bus_driver = {
    .name = "aur_bus",
    .bus = &pci_bus,
    .id_table = aur_pci_id_table,
    .probe = aur_bus_probe,
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void test_aur_sup_step(void) {
  struct aur_sup s;
  memset(&s, 0, sizeof(s));
  strcpy(s.path, "/usr/lib/aur/fake.aur");
  s.state = SUP_RUNNING;
  s.pid = 99;
  s.started = 0;

  // Vivo: nada que hacer.
  TEST_ASSERT(aur_sup_step(&s, 100, -EBUSY, 0) == SUP_NONE &&
                  s.state == SUP_RUNNING,
              "vivo");
  // Muere con fallo: backoff de 500 ms.
  TEST_ASSERT(aur_sup_step(&s, 1000, 0, 139) == SUP_NONE &&
                  s.state == SUP_BACKOFF && s.retry_at == 1500,
              "primer backoff");
  TEST_ASSERT(aur_sup_step(&s, 1499, -EBUSY, 0) == SUP_NONE, "aún en espera");
  TEST_ASSERT(aur_sup_step(&s, 1500, -EBUSY, 0) == SUP_RESTART_NOW,
              "toca relanzar");

  // Relanzado (lo hace el hilo): segundo fallo seguido => backoff doble.
  s.restarts = 1;
  s.state = SUP_RUNNING;
  s.started = 1500;
  aur_sup_step(&s, 2000, 0, 1);
  TEST_ASSERT(s.state == SUP_BACKOFF && s.retry_at == 3000, "backoff doble");

  // Aguantó mucho vivo: el contador se resetea.
  s.state = SUP_RUNNING;
  s.restarts = 4;
  s.started = 0;
  aur_sup_step(&s, AUR_SUP_STABLE_MS + 1, 0, 1);
  TEST_ASSERT(s.restarts == 0 && s.state == SUP_BACKOFF, "estable resetea");

  // Demasiados fallos seguidos: abandonado.
  s.state = SUP_RUNNING;
  s.restarts = AUR_SUP_MAX_RESTARTS;
  s.started = 100;
  aur_sup_step(&s, 200, 0, 1);
  TEST_ASSERT(s.state == SUP_GIVEN_UP, "abandonado");

  // Salida limpia: no se reinicia.
  s.state = SUP_RUNNING;
  s.restarts = 0;
  aur_sup_step(&s, 300, 0, 0);
  TEST_ASSERT(s.state == SUP_FREE, "código 0 libera la entrada");
}
REGISTER_TEST("aur-sup: máquina de estados de reinicio", test_aur_sup_step);