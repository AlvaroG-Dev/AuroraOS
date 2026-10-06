// kernel/tarfs.c
//
// TarFS: sistema de ficheros read-only sobre un tar empaquetado en
// .rodata. Sin escritura, sin creación, sin borrado. Los symlinks
// (typeflag '2') se exponen vía vfs_node.is_symlink/link_target, y el
// VFS genérico decide si seguirlos o no.
//
// Este fichero NO conoce nada de mount table, lookup recursivo, ni
// FDs. Solo implementa las ops a nivel de nodo y el lookup de paths
// relativos al mount.

#include "tarfs.h"
#include "heap.h"
#include "klog.h"
#include "serial.h"
#include "string.h"
#include "uaccess.h" // EISDIR, EROFS, ENOTDIR, EINVAL, ENAMETOOLONG
#include "vfs.h"     // vfs_node_t, vfs_ops_t, vfs_fs_ops_t, vfs_dirent_t

// ===========================================================================
// Parsing del tar
// ===========================================================================
typedef struct {
  char filename[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char chksum[8];
  char typeflag;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char pad[12];
} __attribute__((packed)) ustar_header_t;

static tar_node_t *nodes = NULL;
static size_t node_count = 0;
static size_t node_capacity = 0;

static int tarfs_grow_nodes(size_t required) {
  if (required <= node_capacity)
    return 0;

  size_t new_capacity = node_capacity ? node_capacity : 64;
  while (new_capacity < required) {
    if (new_capacity > (SIZE_MAX / 2))
      return -1;
    new_capacity *= 2;
  }

  tar_node_t *new_nodes =
      (tar_node_t *)krealloc(nodes, new_capacity * sizeof(*nodes));
  if (!new_nodes)
    return -1;

  nodes = new_nodes;
  node_capacity = new_capacity;
  return 0;
}

// ---------------------------------------------------------------------------
// Helpers de paths. Estos operan sobre paths RELATIVOS al tar, no
// paths VFS. Su semántica es distinta de vfs.c:normalize_path.
//   - tarfs_normalize: quita "./" y "/" iniciales.
//   - collapse_path:   resuelve ".." sin salir de raíz, no devuelve '/'.
// ---------------------------------------------------------------------------
static size_t parse_octal(const char *str, size_t max_len) {
  size_t n = 0;
  for (size_t i = 0; i < max_len && str[i] >= '0' && str[i] <= '7'; i++)
    n = (n << 3) + (str[i] - '0');
  return n;
}

static const char *tarfs_normalize(const char *path) {
  while (*path) {
    if (path[0] == '.' && path[1] == '/') {
      path += 2;
    } else if (path[0] == '/') {
      path++;
    } else {
      break;
    }
  }
  return path;
}

static void collapse_path(const char *in, char *out, size_t outlen) {
  if (!in || !out || outlen < 2) {
    if (outlen)
      out[0] = '\0';
    return;
  }

  const char *p = in;
  size_t o = 0;

  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break;

    const char *seg = p;
    while (*p && *p != '/')
      p++;
    size_t seglen = (size_t)(p - seg);

    if (seglen == 1 && seg[0] == '.')
      continue;

    if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
      while (o > 0 && out[o - 1] != '/')
        o--;
      if (o > 0)
        o--;
      continue;
    }

    if (o > 0) {
      if (o + 1 >= outlen)
        break;
      out[o++] = '/';
    }
    if (o + seglen >= outlen)
      break;
    for (size_t i = 0; i < seglen; i++)
      out[o++] = seg[i];
  }
  out[o] = '\0';
}

static tar_node_t *find_by_name(const char *name) {
  for (size_t i = 0; i < node_count; i++) {
    if (strcmp(nodes[i].name, name) == 0)
      return &nodes[i];
  }
  return NULL;
}

// ===========================================================================
// Init y utilidades públicas
// ===========================================================================
void tarfs_init(const void *tar_addr, size_t tar_size) {
  if (nodes) {
    kfree(nodes);
    nodes = NULL;
  }
  node_count = 0;
  node_capacity = 0;

  uint8_t *ptr = (uint8_t *)tar_addr;
  uint8_t *end = ptr + tar_size;
  LOG_INFO("[TARFS] Analizando Initramfs en %p (%lu bytes)...", tar_addr,
           (unsigned long)tar_size);

  while (ptr + 512 <= end) {
    ustar_header_t *hdr = (ustar_header_t *)ptr;

    if (hdr->filename[0] == '\0')
      break;
    if (strncmp(hdr->magic, "ustar", 5) != 0)
      break;

    size_t file_size = parse_octal(hdr->size, sizeof(hdr->size));
    int is_dir = (hdr->typeflag == '5');
    int is_symlink = (hdr->typeflag == '2');

    // [3.4.a] Parsear mode/uid/gid del header ustar (octal).
    uint32_t file_mode = (uint32_t)parse_octal(hdr->mode, sizeof(hdr->mode));
    uint32_t file_uid = (uint32_t)parse_octal(hdr->uid, sizeof(hdr->uid));
    uint32_t file_gid = (uint32_t)parse_octal(hdr->gid, sizeof(hdr->gid));

    char full_path[256];
    size_t pos = 0;

    if (hdr->prefix[0] != '\0') {
      size_t plen = strlen(hdr->prefix);
      for (size_t i = 0; i < plen && pos < 255; i++)
        full_path[pos++] = hdr->prefix[i];
      if (pos < 255)
        full_path[pos++] = '/';
    }
    size_t fnlen = strlen(hdr->filename);
    for (size_t i = 0; i < fnlen && pos < 255; i++)
      full_path[pos++] = hdr->filename[i];
    full_path[pos] = '\0';

    const char *clean_path = tarfs_normalize(full_path);

    if (clean_path[0] != '\0') {
      if (tarfs_grow_nodes(node_count + 1) != 0) {
        LOG_PANIC("[TARFS] No se pudo ampliar el índice de nodos (%lu entradas)",
                  (unsigned long)(node_count + 1));
        return;
      }

      {
        tar_node_t *node = &nodes[node_count++];
        memset(node, 0, sizeof(*node));

        size_t cp_len = strlen(clean_path);
        if (cp_len >= sizeof(node->name))
          cp_len = sizeof(node->name) - 1;
        for (size_t i = 0; i < cp_len; i++)
          node->name[i] = clean_path[i];
        node->name[cp_len] = '\0';

        size_t nlen = strlen(node->name);
        if (nlen > 0 && node->name[nlen - 1] == '/') {
          node->name[nlen - 1] = '\0';
          is_dir = 1;
        }

        node->data = ptr + 512;
        node->size = file_size;
        node->is_dir = is_dir;
        node->is_symlink = is_symlink;

        // [3.4.a] Propagar metadatos del header.
        node->mode = file_mode & 07777;
        node->uid = file_uid;
        node->gid = file_gid;

        if (is_symlink) {
          size_t i;
          for (i = 0; i < sizeof(node->linkname) - 1 && hdr->linkname[i]; i++)
            node->linkname[i] = hdr->linkname[i];
          node->linkname[i] = '\0';
          LOG_TRACE("  [TARFS] <SYML> %s -> %s", node->name, node->linkname);
        } else if (!is_dir) {
          LOG_TRACE("  [TARFS] <FILE> %s (%lu bytes)", node->name,
                    (unsigned long)file_size);
        } else {
          LOG_TRACE("  [TARFS] <DIR>  %s", node->name);
        }
      }
    }

    size_t data_blocks = (file_size + 511) / 512;
    ptr += 512 + (data_blocks * 512);
  }

  LOG_INFO("[TARFS] Carga completa. %lu nodos registrados.",
           (unsigned long)node_count);
}

// Resolución de symlinks del tar (uso interno / legacy).
// El VFS ya no llama aquí para hacer lookup: prefiere find_by_name +
// seguir el symlink él mismo, para no duplicar la lógica de "nofollow".
// Esta función se mantiene porque tar_find_file la usa.
tar_node_t *tarfs_open(const char *path) {
  char current[256];
  const char *norm = tarfs_normalize(path);
  collapse_path(norm, current, sizeof(current));

  for (int depth = 0; depth < 8; depth++) {
    tar_node_t *node = find_by_name(current);
    if (!node)
      return NULL;
    if (!node->is_symlink)
      return node;

    const char *target = node->linkname;
    char next[256];
    if (target[0] == '/') {
      collapse_path(target, next, sizeof(next));
    } else {
      char dir[256];
      size_t clen = strlen(current);
      size_t last_slash = 0;
      for (size_t i = 0; i < clen; i++)
        if (current[i] == '/')
          last_slash = i;
      if (last_slash == 0) {
        dir[0] = '\0';
      } else {
        memcpy(dir, current, last_slash);
        dir[last_slash] = '\0';
      }

      if (dir[0]) {
        size_t dl = strlen(dir);
        if (dl + 1 + strlen(target) < sizeof(next)) {
          memcpy(next, dir, dl);
          next[dl] = '/';
          strcpy(next + dl + 1, target);
        } else {
          next[0] = '\0';
        }
      } else {
        strncpy(next, target, sizeof(next) - 1);
        next[sizeof(next) - 1] = '\0';
      }
      collapse_path(next, current, sizeof(current));
      continue;
    }
    strncpy(current, next, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';
  }

  LOG_WARN("[TARFS] Symlink loop en '%s'", path);
  return NULL;
}

void tarfs_list(const char *dir_path) {
  const char *target = tarfs_normalize(dir_path);
  size_t tlen = strlen(target);

  LOG_TRACE("[TARFS] Listando directorio: '/%s':", target);

  for (size_t i = 0; i < node_count; i++) {
    if (tlen == 0 ||
        (strncmp(nodes[i].name, target, tlen) == 0 &&
         (nodes[i].name[tlen] == '/' || nodes[i].name[tlen] == '\0'))) {
      const char *kind = nodes[i].is_dir       ? "[DIR]  "
                         : nodes[i].is_symlink ? "[SYML] "
                                               : "[FILE] ";
      LOG_TRACE("  - %s%s", kind, nodes[i].name);
    }
  }
}

size_t tarfs_get_node_count(void) { return node_count; }

tar_node_t *tarfs_get_node(size_t index) {
  if (index >= node_count)
    return NULL;
  return &nodes[index];
}

tar_node_t *tar_find_file(const char *path) { return tarfs_open(path); }

// ===========================================================================
// Node ops (vfs_ops_t)
// ===========================================================================
static int64_t tar_vfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                            void *buf) {
  if (!node || !node->priv || !buf)
    return -EINVAL;
  tar_node_t *tn = (tar_node_t *)node->priv;
  if (tn->is_dir)
    return -EISDIR;
  if (offset >= tn->size)
    return 0;

  size_t to_read = size;
  if (offset + to_read > tn->size)
    to_read = tn->size - offset;
  memcpy(buf, tn->data + offset, to_read);
  return (int64_t)to_read;
}

static int64_t tar_vfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                             const void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return -EROFS;
}

static int tar_vfs_open(vfs_node_t *node, int flags) {
  (void)node;
  // tarfs es read-only.
  if ((flags & O_WRONLY) || (flags & O_RDWR))
    return -EROFS;
  return 0;
}

static int tar_vfs_close(vfs_node_t *node) {
  (void)node;
  return 0;
}

// [3.2] Hard links no soportados en tarfs (RO). Devolvemos EROFS
// explícito para que vfs_link no tenga que adivinar el tipo de FS.
static int tar_vfs_link(vfs_node_t *dir, const char *name, vfs_node_t *target) {
  (void)dir;
  (void)name;
  (void)target;
  return -EROFS;
}

// readdir: recorre todos los nodos del tar y devuelve los hijos
// directos del directorio `dir`. Un hijo es directo si:
//   - su nombre empieza por "<dir_name>/" (o por nada, si es raíz)
//   - el resto no contiene '/'
static int tar_vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  if (!dir || !dir->priv || !out)
    return -EINVAL;
  tar_node_t *tn = (tar_node_t *)dir->priv;
  if (!tn->is_dir)
    return -ENOTDIR;

  size_t plen = strlen(tn->name);
  char prefix[260];
  size_t prefix_len;
  if (plen == 0) {
    prefix[0] = '\0';
    prefix_len = 0;
  } else {
    if (plen + 2 > sizeof(prefix))
      return -ENAMETOOLONG;
    memcpy(prefix, tn->name, plen);
    prefix[plen] = '/';
    prefix[plen + 1] = '\0';
    prefix_len = plen + 1;
  }

  size_t n = tarfs_get_node_count();
  uint64_t seen = 0;
  for (size_t i = 0; i < n; i++) {
    tar_node_t *child = tarfs_get_node(i);
    if (!child || child == tn)
      continue;

    const char *name = child->name;
    if (prefix_len > 0) {
      if (strncmp(name, prefix, prefix_len) != 0)
        continue;
      name += prefix_len;
    }
    if (*name == '\0')
      continue;
    int has_slash = 0;
    for (const char *p = name; *p; p++) {
      if (*p == '/') {
        has_slash = 1;
        break;
      }
    }
    if (has_slash)
      continue;

    if (seen == index) {
      size_t rlen = strlen(name);
      if (rlen >= sizeof(out->name))
        rlen = sizeof(out->name) - 1;
      for (size_t k = 0; k < rlen; k++)
        out->name[k] = name[k];
      out->name[rlen] = '\0';
      out->type = child->is_dir ? VFS_DIRECTORY : VFS_FILE;
      out->size = child->is_dir ? 0 : child->size;
      return 0;
    }
    seen++;
  }

  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;
  return 0;
}

// Nodo sintético para la raíz de tarfs. tarfs no tiene entry para "".
// Vive en .data y no se libera nunca.
static tar_node_t g_tarfs_root = {
    .name = "",
    .data = NULL,
    .size = 0,
    .is_dir = 1,
};

static vfs_ops_t tar_file_ops = {
    .read = tar_vfs_read,
    .write = tar_vfs_write,
    .open = tar_vfs_open,
    .close = tar_vfs_close,
    .readable = NULL,
    // [3.4.c] RO: chmod/chown devuelven -EROFS.
    .chmod = NULL,
    .chown = NULL,
};

static vfs_ops_t tar_dir_ops = {
    .read = tar_vfs_read,
    .write = tar_vfs_write,
    .open = tar_vfs_open,
    .close = tar_vfs_close,
    .readable = NULL,
    .readdir = tar_vfs_readdir,
    .link = tar_vfs_link, // [3.2]
    // [3.4.c] RO: chmod/chown devuelven -EROFS.
    .chmod = NULL,
    .chown = NULL,
};

// ===========================================================================
// fs lookup (vfs_fs_ops_t)
//
// Path RELATIVO al mount (empieza por '/'). NUNCA sigue symlinks:
// devuelve el nodo del symlink con is_symlink=1 y link_target relleno.
// vfs_lookup_rec() decide si seguirlo.
// ===========================================================================
static vfs_node_t *tarfs_fs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;

  // Caso raíz: directorio sintético sobre g_tarfs_root.
  if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
    vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!node)
      return NULL;
    node->name[0] = '/';
    node->name[1] = '\0';
    node->flags = VFS_DIRECTORY;
    node->size = 0;
    node->inode = 0;
    node->ops = &tar_dir_ops;
    node->fs = NULL;
    node->priv = &g_tarfs_root;
    // [3.4.a] Raíz tarfs: root:root 0755.
    node->mode = S_IFDIR | 0755;
    node->uid = 0;
    node->gid = 0;
    return node;
  }

  char collapsed[256];
  const char *norm = tarfs_normalize(path);
  collapse_path(norm, collapsed, sizeof(collapsed));

  tar_node_t *tn = find_by_name(collapsed);
  if (!tn)
    return NULL;

  vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;

  size_t nlen = strlen(tn->name);
  if (nlen >= sizeof(node->name))
    nlen = sizeof(node->name) - 1;
  memcpy(node->name, tn->name, nlen);
  node->name[nlen] = '\0';

  node->flags = tn->is_dir ? VFS_DIRECTORY : VFS_FILE;
  node->size = tn->size;
  node->inode = 0;
  node->ops = tn->is_dir ? &tar_dir_ops : &tar_file_ops;
  node->priv = tn;

  // [3.1] Exponer el symlink si lo es.
  if (tn->is_symlink) {
    node->is_symlink = 1;
    size_t tlen = strlen(tn->linkname);
    if (tlen >= sizeof(node->link_target))
      tlen = sizeof(node->link_target) - 1;
    memcpy(node->link_target, tn->linkname, tlen);
    node->link_target[tlen] = '\0';
  }

  // [3.4.a] Propagar mode/uid/gid. Los S_IF* los añadimos aquí.
  if (tn->is_symlink) {
    node->mode = S_IFLNK | 0777;
  } else if (tn->is_dir) {
    node->mode = S_IFDIR | (tn->mode & 07777);
  } else {
    node->mode = S_IFREG | (tn->mode & 07777);
  }
  node->uid = tn->uid;
  node->gid = tn->gid;

  return node;
}

// [4.1] tarfs es in-memory. Reportamos bloques = 0 (nada contable) y
// files = número de nodos del tar.
static int tarfs_statfs(void *fs_priv, struct vfs_statfs *out) {
  (void)fs_priv;
  memset(out, 0, sizeof(*out));
  out->f_type = 0x01021994; // TMPFS_MAGIC
  out->f_bsize = 512;
  out->f_frsize = 512;
  out->f_blocks = 0;
  out->f_bfree = 0;
  out->f_bavail = 0;
  out->f_files = tarfs_get_node_count();
  out->f_ffree = 0;
  out->f_namelen = 255;
  return 0;
}

static vfs_fs_ops_t tarfs_fs_ops = {
    .lookup = tarfs_fs_lookup,
    .statfs = tarfs_statfs,
    .name = "tarfs",
};

struct vfs_fs_ops *tarfs_get_vfs_ops(void) {
  return (struct vfs_fs_ops *)&tarfs_fs_ops;
}