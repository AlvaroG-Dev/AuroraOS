// kernel/sysfs_dyn.c — ver sysfs_dyn.h

#include "sysfs_dyn.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "string.h"
#include "uaccess.h"

#define SD_MAX 512
#define SD_PATH_MAX 112

typedef struct {
  bool used;
  uint8_t kind; // SYSFS_DYN_FILE / LINK
  char path[SD_PATH_MAX];
  char *data;
  size_t len;
} sd_entry_t;

static sd_entry_t g_ent[SD_MAX];
static mutex_t g_sd_lock;

static int add_locked(const char *path, int kind, const char *data) {
  size_t pl = strlen(path);
  if (!pl || pl >= SD_PATH_MAX || path[0] == '/')
    return -EINVAL;
  size_t dl = strlen(data);
  char *copy = (char *)kmalloc(dl + 1);
  if (!copy)
    return -ENOMEM;
  memcpy(copy, data, dl + 1);
  int slot = -1;
  for (int i = 0; i < SD_MAX; i++) {
    if (g_ent[i].used && strcmp(g_ent[i].path, path) == 0) {
      kfree(g_ent[i].data);
      g_ent[i].data = copy;
      g_ent[i].len = dl;
      g_ent[i].kind = (uint8_t)kind;
      return 0;
    }
    if (!g_ent[i].used && slot < 0)
      slot = i;
  }
  if (slot < 0) {
    kfree(copy);
    return -ENOSPC;
  }
  memset(&g_ent[slot], 0, sizeof(g_ent[slot]));
  memcpy(g_ent[slot].path, path, pl + 1);
  g_ent[slot].data = copy;
  g_ent[slot].len = dl;
  g_ent[slot].kind = (uint8_t)kind;
  g_ent[slot].used = true;
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

int sysfs_dyn_remove_tree(const char *prefix) {
  size_t pl = strlen(prefix);
  int n = 0;
  mutex_lock(&g_sd_lock);
  for (int i = 0; i < SD_MAX; i++) {
    if (!g_ent[i].used)
      continue;
    if (strncmp(g_ent[i].path, prefix, pl) == 0 &&
        (g_ent[i].path[pl] == '\0' || g_ent[i].path[pl] == '/')) {
      kfree(g_ent[i].data);
      g_ent[i].data = NULL;
      g_ent[i].used = false;
      n++;
    }
  }
  mutex_unlock(&g_sd_lock);
  return n;
}

int sysfs_dyn_get(const char *path, char **data, size_t *len) {
  size_t pl = strlen(path);
  int kind = SYSFS_DYN_NONE;
  *data = NULL;
  *len = 0;
  mutex_lock(&g_sd_lock);
  for (int i = 0; i < SD_MAX; i++) {
    if (!g_ent[i].used)
      continue;
    if (strcmp(g_ent[i].path, path) == 0) {
      char *c = (char *)kmalloc(g_ent[i].len + 1);
      if (c) {
        memcpy(c, g_ent[i].data, g_ent[i].len + 1);
        *data = c;
        *len = g_ent[i].len;
        kind = g_ent[i].kind;
      }
      break;
    }
    if (strncmp(g_ent[i].path, path, pl) == 0 && g_ent[i].path[pl] == '/')
      kind = SYSFS_DYN_DIR;
  }
  mutex_unlock(&g_sd_lock);
  return kind;
}

int sysfs_dyn_readdir(const char *dir, uint64_t index, vfs_dirent_t *out) {
  size_t dl = strlen(dir);
  uint64_t seen = 0;
  bool known = (dl == 0);
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;

  mutex_lock(&g_sd_lock);
  for (int i = 0; i < SD_MAX; i++) {
    if (!g_ent[i].used)
      continue;
    const char *p = g_ent[i].path;
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
    out->type = sub ? VFS_DIRECTORY
                    : (g_ent[i].kind == SYSFS_DYN_LINK ? VFS_SYMLINK_ENT : VFS_FILE);
    out->size = sub ? 0 : g_ent[i].len;
    break;
  }
  mutex_unlock(&g_sd_lock);
  return known ? 0 : -ENOENT;
}
