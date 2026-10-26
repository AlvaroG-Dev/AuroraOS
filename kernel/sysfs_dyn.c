// kernel/sysfs_dyn.c — ver sysfs_dyn.h

#include "sysfs_dyn.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "sched.h"
#include "string.h"
#include "uaccess.h"

// Las entradas van en un array de punteros que crece a demanda (cada entrada
// es un bloque propio: un atributo en uso sigue siendo valido aunque el
// array se realoje). Antes era un array fijo de 512 entradas.
#define SD_MAX 16384

typedef struct {
  uint8_t kind; // SYSFS_DYN_FILE / LINK / ATTR
  uint8_t dying; // ATTR: se esta quitando, no admite llamadas nuevas
  uint32_t mode;
  char path[SYSFS_DYN_PATH_MAX];
  char *data;
  size_t len;
  const sysfs_dyn_attr_ops_t *ops;
  void *ctx;
  int active; // ATTR: llamadas a show/store en curso
} sd_entry_t;

static sd_entry_t **g_ent;
static int g_cap;
static int g_n; // entradas usadas: g_ent[0..g_n-1], sin huecos
static mutex_t g_sd_lock;

static int find_locked(const char *path) {
  for (int i = 0; i < g_n; i++)
    if (strcmp(g_ent[i]->path, path) == 0)
      return i;
  return -1;
}

static sd_entry_t *new_entry_locked(const char *path) {
  if (g_n == g_cap) {
    int ncap = g_cap ? g_cap * 2 : 256;
    if (ncap > SD_MAX)
      return NULL;
    sd_entry_t **n = (sd_entry_t **)kmalloc(sizeof(*n) * (size_t)ncap);
    if (!n)
      return NULL;
    if (g_n)
      memcpy(n, g_ent, sizeof(*n) * (size_t)g_n);
    kfree(g_ent);
    g_ent = n;
    g_cap = ncap;
  }
  sd_entry_t *e = (sd_entry_t *)kzalloc(sizeof(*e));
  if (!e)
    return NULL;
  memcpy(e->path, path, strlen(path) + 1);
  g_ent[g_n++] = e;
  return e;
}

static int path_ok(const char *path) {
  size_t pl = strlen(path);
  return pl && pl < SYSFS_DYN_PATH_MAX && path[0] != '/';
}

static int add_locked(const char *path, int kind, const char *data) {
  if (!path_ok(path))
    return -EINVAL;
  size_t dl = strlen(data);
  char *copy = (char *)kmalloc(dl + 1);
  if (!copy)
    return -ENOMEM;
  memcpy(copy, data, dl + 1);
  int i = find_locked(path);
  if (i >= 0 && g_ent[i]->kind != SYSFS_DYN_ATTR) {
    kfree(g_ent[i]->data);
    g_ent[i]->data = copy;
    g_ent[i]->len = dl;
    g_ent[i]->kind = (uint8_t)kind;
    return 0;
  }
  if (i >= 0) {
    kfree(copy);
    return -EEXIST;
  }
  sd_entry_t *e = new_entry_locked(path);
  if (!e) {
    kfree(copy);
    return -ENOSPC;
  }
  e->data = copy;
  e->len = dl;
  e->kind = (uint8_t)kind;
  e->mode = kind == SYSFS_DYN_LINK ? 0777 : 0444;
  return 0;
}

int sysfs_dyn_set_file(const char *path, const char *content) {
  mutex_lock(&g_sd_lock);
  int rc = add_locked(path, SYSFS_DYN_FILE, content);
  mutex_unlock(&g_sd_lock);
  return rc;
}

int sysfs_dyn_add_symlink(const char *path, const char *target) {
  mutex_lock(&g_sd_lock);
  int rc = add_locked(path, SYSFS_DYN_LINK, target);
  mutex_unlock(&g_sd_lock);
  return rc;
}

int sysfs_dyn_add_attr(const char *path, const sysfs_dyn_attr_ops_t *ops,
                       void *ctx, uint32_t mode) {
  if (!path_ok(path) || !ops)
    return -EINVAL;
  mutex_lock(&g_sd_lock);
  int rc = 0;
  if (find_locked(path) >= 0) {
    rc = -EEXIST;
  } else {
    sd_entry_t *e = new_entry_locked(path);
    if (!e) {
      rc = -ENOSPC;
    } else {
      e->kind = SYSFS_DYN_ATTR;
      e->mode = mode & 0777;
      e->ops = ops;
      e->ctx = ctx;
    }
  }
  mutex_unlock(&g_sd_lock);
  return rc;
}

// Quita g_ent[i] (con el cerrojo). Un atributo con llamadas en curso se marca
// y se espera a que terminen; devuelve con el cerrojo tomado otra vez y la
// entrada ya fuera del array. Devuelve 1 si tuvo que soltar el cerrojo (el
// array puede haber cambiado entretanto).
static int remove_at_locked(int i) {
  sd_entry_t *e = g_ent[i];
  int waited = 0;
  g_ent[i] = g_ent[--g_n];
  if (e->kind == SYSFS_DYN_ATTR) {
    e->dying = 1;
    while (__atomic_load_n(&e->active, __ATOMIC_ACQUIRE) != 0) {
      mutex_unlock(&g_sd_lock);
      sched_yield();
      mutex_lock(&g_sd_lock);
      waited = 1;
    }
  }
  kfree(e->data);
  kfree(e);
  return waited;
}

int sysfs_dyn_remove(const char *path) {
  mutex_lock(&g_sd_lock);
  int i = find_locked(path);
  if (i >= 0)
    remove_at_locked(i);
  mutex_unlock(&g_sd_lock);
  return i >= 0 ? 0 : -ENOENT;
}

int sysfs_dyn_remove_tree(const char *prefix) {
  size_t pl = strlen(prefix);
  int n = 0;
  mutex_lock(&g_sd_lock);
  for (int i = 0; i < g_n;) {
    const char *p = g_ent[i]->path;
    if (strncmp(p, prefix, pl) == 0 && (p[pl] == '\0' || p[pl] == '/')) {
      if (remove_at_locked(i)) // mueve la ultima a i: no avanzar
        i = 0;
      n++;
    } else {
      i++;
    }
  }
  mutex_unlock(&g_sd_lock);
  return n;
}

// Toma una referencia de llamada sobre el atributo `path` (con el cerrojo).
static sd_entry_t *attr_get_locked(const char *path) {
  int i = find_locked(path);
  if (i < 0 || g_ent[i]->kind != SYSFS_DYN_ATTR || g_ent[i]->dying)
    return NULL;
  __atomic_add_fetch(&g_ent[i]->active, 1, __ATOMIC_ACQ_REL);
  return g_ent[i];
}

static void attr_put(sd_entry_t *e) {
  __atomic_sub_fetch(&e->active, 1, __ATOMIC_ACQ_REL);
}

int sysfs_dyn_get(const char *path, char **data, size_t *len) {
  size_t pl = strlen(path);
  int kind = SYSFS_DYN_NONE;
  sd_entry_t *attr = NULL;
  *data = NULL;
  *len = 0;
  mutex_lock(&g_sd_lock);
  for (int i = 0; i < g_n; i++) {
    sd_entry_t *e = g_ent[i];
    if (strcmp(e->path, path) == 0) {
      if (e->kind == SYSFS_DYN_ATTR) {
        attr = attr_get_locked(path);
        kind = attr ? SYSFS_DYN_ATTR : SYSFS_DYN_NONE;
        break;
      }
      char *c = (char *)kmalloc(e->len + 1);
      if (c) {
        memcpy(c, e->data, e->len + 1);
        *data = c;
        *len = e->len;
        kind = e->kind;
      }
      break;
    }
    if (strncmp(e->path, path, pl) == 0 && e->path[pl] == '/')
      kind = SYSFS_DYN_DIR;
  }
  mutex_unlock(&g_sd_lock);

  if (attr) {
    char *buf = (char *)kzalloc(SYSFS_DYN_ATTR_BUF + 1);
    long n = 0;
    if (buf && attr->ops->show && (attr->mode & 0444)) {
      n = attr->ops->show(attr->ctx, buf, SYSFS_DYN_ATTR_BUF);
      if (n < 0 || n > SYSFS_DYN_ATTR_BUF)
        n = 0;
    }
    attr_put(attr);
    if (!buf)
      return SYSFS_DYN_NONE;
    buf[n] = '\0';
    *data = buf;
    *len = (size_t)n;
  }
  return kind;
}

uint32_t sysfs_dyn_mode(const char *path) {
  mutex_lock(&g_sd_lock);
  int i = find_locked(path);
  uint32_t m = i >= 0 ? g_ent[i]->mode : 0555;
  mutex_unlock(&g_sd_lock);
  return m;
}

long sysfs_dyn_store(const char *path, const char *buf, size_t len) {
  mutex_lock(&g_sd_lock);
  sd_entry_t *e = attr_get_locked(path);
  mutex_unlock(&g_sd_lock);
  if (!e)
    return -ENOENT;
  long rc = -EACCES;
  if (e->ops->store && (e->mode & 0222))
    rc = e->ops->store(e->ctx, buf, len);
  attr_put(e);
  return rc;
}

int sysfs_dyn_readdir(const char *dir, uint64_t index, vfs_dirent_t *out) {
  size_t dl = strlen(dir);
  uint64_t seen = 0;
  bool known = (dl == 0);
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;

  mutex_lock(&g_sd_lock);
  for (int i = 0; i < g_n; i++) {
    const char *p = g_ent[i]->path;
    if (dl) {
      if (strncmp(p, dir, dl) != 0 || p[dl] != '/')
        continue;
      p += dl + 1;
    }
    known = true;
    const char *slash = p;
    while (*slash && *slash != '/')
      slash++;
    size_t cl = (size_t)(slash - p);
    bool sub = (*slash == '/');
    if (sub) { // directorio implicito: solo la primera vez
      bool dup = false;
      for (int j = 0; j < i; j++) {
        const char *q = g_ent[j]->path;
        if (dl) {
          if (strncmp(q, dir, dl) != 0 || q[dl] != '/')
            continue;
          q += dl + 1;
        }
        if (strncmp(q, p, cl) == 0 && q[cl] == '/') {
          dup = true;
          break;
        }
      }
      if (dup)
        continue;
    }
    if (seen++ != index)
      continue;
    if (cl >= sizeof(out->name))
      cl = sizeof(out->name) - 1;
    memcpy(out->name, p, cl);
    out->name[cl] = '\0';
    out->type = sub ? VFS_DIRECTORY
                    : (g_ent[i]->kind == SYSFS_DYN_LINK ? VFS_SYMLINK_ENT
                                                         : VFS_FILE);
    out->size = sub ? 0 : g_ent[i]->len;
    break;
  }
  mutex_unlock(&g_sd_lock);
  return known ? 0 : -ENOENT;
}
