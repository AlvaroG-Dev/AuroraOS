// kernel/kmod_sys.c
//
// [FASE 3.4] Capa de compatibilidad Linux para módulos de kernel.
//
// kmod (insmod/modprobe/rmmod/lsmod) habla con el kernel así:
//   insmod / modprobe  -> finit_module(fd, params, flags)  (o init_module)
//   rmmod / modprobe -r -> delete_module(name, flags)
//   lsmod               -> /proc/modules
//   (todos)             -> /sys/module/<m>/{initstate,refcnt,holders}
//
// Los "módulos" son .kur (ver kur.h). El fichero que kmod abre puede llamarse
// .ko (así lo indexa depmod); al kernel solo le importa el contenido.
// Los parámetros de módulo (uargs) se aceptan e ignoran: un .kur no los tiene.

#include "kmod_sys.h"
#include "heap.h"
#include "klog.h"
#include "kur.h"
#include "kur_loader.h"
#include "process.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

#define MODULE_INIT_IGNORE_MODVERSIONS 1
#define MODULE_INIT_IGNORE_VERMAGIC 2
#define MODULE_INIT_COMPRESSED_FILE 4
#define MODULE_INIT_KNOWN                                                      \
  (MODULE_INIT_IGNORE_MODVERSIONS | MODULE_INIT_IGNORE_VERMAGIC |              \
   MODULE_INIT_COMPRESSED_FILE)

#define KMOD_O_NONBLOCK 04000
#define KMOD_O_TRUNC 01000

static int kmod_may_load(void) {
  process_t *p = process_current();
  return p && p->euid == 0;
}

static int64_t kmod_load_buffer(const void *data, size_t size) {
  struct kur_module *m = NULL;
  int rc = kur_loader_load_buffer(data, size, &m);
  if (rc < 0)
    LOG_WARN("[KMOD] carga rechazada: %d", rc);
  else
    LOG_INFO("[KMOD] módulo '%s' cargado", m ? m->name : "?");
  return rc < 0 ? rc : 0;
}

int64_t k_init_module(uint64_t umod, uint64_t len, uint64_t uargs, uint64_t a4,
                      uint64_t a5) {
  (void)uargs;
  (void)a4;
  (void)a5;
  if (!kmod_may_load())
    return -EPERM;
  if (len == 0 || len > KUR_MAX_SIZE)
    return -EINVAL;
  if (!access_ok((void *)umod, len))
    return -EFAULT;

  void *buf = kmalloc(len);
  if (!buf)
    return -ENOMEM;
  if (copy_from_user(buf, (void *)umod, len) < 0) {
    kfree(buf);
    return -EFAULT;
  }
  int64_t rc = kmod_load_buffer(buf, len);
  kfree(buf);
  return rc;
}

int64_t k_finit_module(uint64_t fd, uint64_t uargs, uint64_t flags,
                       uint64_t a4, uint64_t a5) {
  (void)uargs;
  (void)a4;
  (void)a5;
  if (!kmod_may_load())
    return -EPERM;
  if (flags & ~(uint64_t)MODULE_INIT_KNOWN)
    return -EINVAL;
  if (flags & MODULE_INIT_COMPRESSED_FILE)
    return -ENOEXEC; // kmod descomprime él mismo si el kernel no lo soporta

  process_t *proc = process_current();
  if (!proc || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -EBADF;
  vfs_node_t *node = proc->fds[fd]->node;
  if (!node || !node->ops || !node->ops->read || (node->flags & VFS_DIRECTORY))
    return -EINVAL;

  size_t size = node->size;
  if (size == 0 || size > KUR_MAX_SIZE)
    return -ENOEXEC;
  uint8_t *buf = (uint8_t *)kmalloc(size);
  if (!buf)
    return -ENOMEM;

  size_t got = 0;
  while (got < size) {
    int64_t n = node->ops->read(node, got, size - got, buf + got);
    if (n < 0) {
      kfree(buf);
      return n;
    }
    if (n == 0)
      break;
    got += (size_t)n;
  }
  int64_t rc = got == size ? kmod_load_buffer(buf, size) : -EIO;
  kfree(buf);
  return rc;
}

int64_t k_delete_module(uint64_t uname, uint64_t flags, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!kmod_may_load())
    return -EPERM;

  char name[KUR_MAX_NAME];
  long n = strncpy_from_user(name, (const char *)uname, sizeof(name));
  if (n < 0)
    return -EFAULT;
  if (n >= (long)sizeof(name))
    return -ENOENT; // nombre demasiado largo: no puede existir

  struct kur_module *m = kur_loader_find_by_name(name);
  if (!m)
    return -ENOENT;
  // O_TRUNC (rmmod -f) no se honra: descargar un módulo en uso rompería la
  // tabla de símbolos de sus dependientes. O_NONBLOCK es lo normal; no hay
  // espera, así que ambos casos devuelven -EBUSY.
  (void)flags;
  int rc = kur_loader_unload(m);
  if (rc == 0)
    LOG_INFO("[KMOD] módulo '%s' descargado", name);
  return rc;
}

// ---------------------------------------------------------------------------
// Vistas
// ---------------------------------------------------------------------------
static int imports_from(const struct kur_module *user,
                        const struct kur_module *provider) {
  return user != provider && user->n_imports_from[provider->index] > 0;
}

char *kmod_gen_proc_modules(size_t *out_len) {
  // Línea: "<name> <size> <refcnt> <holders|-> Live 0x<addr>\n"
  size_t cap = 64 + (size_t)KUR_MAX_MODULES * (KUR_MAX_NAME + 40 +
                                               (size_t)KUR_MAX_MODULES *
                                                   (KUR_MAX_NAME + 1));
  char *buf = (char *)kmalloc(cap);
  if (!buf)
    return NULL;
  size_t len = 0;
  buf[0] = '\0';

  for (int i = 0; i < KUR_MAX_MODULES; i++) {
    struct kur_module *m = kur_loader_get(i);
    if (!m)
      continue;
    len += (size_t)snprintf(buf + len, cap - len, "%s %lu %u ", m->name,
                            (unsigned long)m->base_size, m->n_dependents);
    int any = 0;
    for (int j = 0; j < KUR_MAX_MODULES; j++) {
      struct kur_module *u = kur_loader_get(j);
      if (u && imports_from(u, m)) {
        len += (size_t)snprintf(buf + len, cap - len, "%s,", u->name);
        any = 1;
      }
    }
    if (!any)
      len += (size_t)snprintf(buf + len, cap - len, "-");
    len += (size_t)snprintf(buf + len, cap - len, " Live 0x%016lx\n",
                            (unsigned long)m->base_va);
  }
  *out_len = len;
  return buf;
}

int kmod_is_loaded(const char *name) {
  return kur_loader_find_by_name(name) != NULL;
}

const char *kmod_loaded_name(uint64_t index) {
  uint64_t seen = 0;
  for (int i = 0; i < KUR_MAX_MODULES; i++) {
    struct kur_module *m = kur_loader_get(i);
    if (!m)
      continue;
    if (seen++ == index)
      return m->name;
  }
  return NULL;
}

const char *kmod_holder_name(const char *module, uint64_t index) {
  struct kur_module *p = kur_loader_find_by_name(module);
  if (!p)
    return NULL;
  uint64_t seen = 0;
  for (int i = 0; i < KUR_MAX_MODULES; i++) {
    struct kur_module *u = kur_loader_get(i);
    if (u && imports_from(u, p) && seen++ == index)
      return u->name;
  }
  return NULL;
}

char *kmod_gen_sysfs_attr(const char *module, const char *attr,
                          size_t *out_len) {
  struct kur_module *m = kur_loader_find_by_name(module);
  if (!m)
    return NULL;
  char *buf = (char *)kmalloc(32);
  if (!buf)
    return NULL;
  int n;
  if (strcmp(attr, "initstate") == 0)
    n = snprintf(buf, 32, "live\n");
  else if (strcmp(attr, "refcnt") == 0)
    n = snprintf(buf, 32, "%u\n", m->n_dependents);
  else if (strcmp(attr, "taint") == 0)
    n = snprintf(buf, 32, "\n");
  else {
    kfree(buf);
    return NULL;
  }
  *out_len = (size_t)n;
  return buf;
}
