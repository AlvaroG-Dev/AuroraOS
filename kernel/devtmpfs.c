// kernel/devtmpfs.c
//
// Registro dinámico de char devices para /dev. Ver devtmpfs.h.

#include "devtmpfs.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "string.h"
#include "uaccess.h"

#define DT_MAX 192
#define DT_PATH_MAX 64

typedef struct {
  bool used;
  char path[DT_PATH_MAX];
  uint32_t rdev;
  uint32_t mode;
  uint32_t uid;
  uint32_t gid;
  vfs_ops_t *ops;
  void *priv;
} dt_entry_t;

static dt_entry_t g_ent[DT_MAX];
static mutex_t g_dt_lock;

static bool valid_path(const char *p) {
  if (!p || !p[0] || p[0] == '/')
    return false;
  size_t n = strlen(p);
  if (n >= DT_PATH_MAX || p[n - 1] == '/')
    return false;
  return true;
}

int devtmpfs_register(const char *path, uint32_t rdev, uint32_t mode,
                      uint32_t uid, uint32_t gid, vfs_ops_t *ops, void *priv) {
  if (!valid_path(path))
    return -EINVAL;
  int rc = 0;
  mutex_lock(&g_dt_lock);
  int slot = -1;
  for (int i = 0; i < DT_MAX; i++) {
    if (g_ent[i].used) {
      if (strcmp(g_ent[i].path, path) == 0) {
        rc = -EEXIST;
        break;
      }
    } else if (slot < 0) {
      slot = i;
    }
  }
  if (rc == 0 && slot < 0)
    rc = -ENOSPC;
  if (rc == 0) {
    dt_entry_t *e = &g_ent[slot];
    memset(e, 0, sizeof(*e));
    strcpy(e->path, path);
    e->rdev = rdev;
    e->mode = mode & 07777;
    e->uid = uid;
    e->gid = gid;
    e->ops = ops;
    e->priv = priv;
    e->used = true;
  }
  mutex_unlock(&g_dt_lock);
  if (rc == 0)
    LOG_INFO("[DEVTMPFS] /dev/%s (%u:%u)", path, rdev >> 8, rdev & 0xFF);
  return rc;
}

int devtmpfs_unregister(const char *path) {
  if (!valid_path(path))
    return -EINVAL;
  int rc = -ENOENT;
  mutex_lock(&g_dt_lock);
  for (int i = 0; i < DT_MAX; i++) {
    if (g_ent[i].used && strcmp(g_ent[i].path, path) == 0) {
      g_ent[i].used = false;
      rc = 0;
      break;
    }
  }
  mutex_unlock(&g_dt_lock);
  return rc;
}

static void fill_name(vfs_node_t *n, const char *rel) {
  size_t l = strlen(rel);
  if (l >= sizeof(n->name))
    l = sizeof(n->name) - 1;
  memcpy(n->name, rel, l);
  n->name[l] = '\0';
}

vfs_node_t *devtmpfs_lookup(const char *rel, vfs_ops_t *dir_ops) {
  if (!valid_path(rel))
    return NULL;
  size_t rl = strlen(rel);
  dt_entry_t found;
  bool have = false, is_dir = false;

  mutex_lock(&g_dt_lock);
  for (int i = 0; i < DT_MAX; i++) {
    if (!g_ent[i].used)
      continue;
    if (strcmp(g_ent[i].path, rel) == 0) {
      found = g_ent[i];
      have = true;
      break;
    }
    if (strncmp(g_ent[i].path, rel, rl) == 0 && g_ent[i].path[rl] == '/')
      is_dir = true;
  }
  mutex_unlock(&g_dt_lock);

  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(*n));
  if (!n)
    return NULL;
  fill_name(n, rel);
  if (have) {
    n->flags = VFS_CHARDEVICE;
    n->ops = found.ops;
    n->priv = found.priv;
    n->mode = S_IFCHR | found.mode;
    n->uid = found.uid;
    n->gid = found.gid;
    n->rdev = found.rdev;
    return n;
  }
  if (is_dir) {
    n->flags = VFS_DIRECTORY;
    n->ops = dir_ops;
    n->mode = S_IFDIR | 0755;
    return n;
  }
  kfree(n);
  return NULL;
}

bool devtmpfs_exists(const char *rel) {
  vfs_node_t *n = devtmpfs_lookup(rel, NULL);
  if (!n)
    return false;
  kfree(n);
  return true;
}

int devtmpfs_readdir(const char *dir, uint64_t index, vfs_dirent_t *out) {
  size_t dl = dir ? strlen(dir) : 0;
  uint64_t seen = 0;
  bool dir_known = (dl == 0);
  int rc = 0;

  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;

  mutex_lock(&g_dt_lock);
  for (int i = 0; i < DT_MAX; i++) {
    if (!g_ent[i].used)
      continue;
    const char *p = g_ent[i].path;
    if (dl) {
      if (strncmp(p, dir, dl) != 0 || p[dl] != '/')
        continue;
      p += dl + 1;
    }
    dir_known = true;
    const char *slash = p;
    while (*slash && *slash != '/')
      slash++;
    size_t cl = (size_t)(slash - p);
    bool sub = (*slash == '/');

    // Dedupe de subdirectorios implícitos: solo el primero.
    if (sub) {
      bool dup = false;
      for (int j = 0; j < i; j++) {
        if (!g_ent[j].used)
          continue;
        const char *q = g_ent[j].path;
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
    out->type = sub ? VFS_DIRECTORY : VFS_CHARDEVICE;
    break;
  }
  if (!dir_known)
    rc = -ENOENT;
  mutex_unlock(&g_dt_lock);
  return rc;
}

bool devtmpfs_find_by_rdev(uint32_t rdev, vfs_ops_t **ops, void **priv) {
  bool ok = false;
  mutex_lock(&g_dt_lock);
  for (int i = 0; i < DT_MAX; i++) {
    if (g_ent[i].used && g_ent[i].rdev == rdev) {
      *ops = g_ent[i].ops;
      *priv = g_ent[i].priv;
      ok = true;
      break;
    }
  }
  mutex_unlock(&g_dt_lock);
  return ok;
}
