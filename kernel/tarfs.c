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

// Forward declaration. tarfs_build_dir_index (más arriba en el fichero)
// llama a find_by_name, pero la definición está más abajo.
static tar_node_t *find_by_name(const char *name);

// Nodo sintético para la raíz de tarfs. Lo usan tar_vfs_readdir y
// tarfs_make_vfs_node. Definición completa, aquí arriba, para que
// esté disponible antes de cualquier uso.
static tar_node_t g_tarfs_root = {
    .name = "",
    .data = NULL,
    .size = 0,
    .is_dir = 1,
};

// Índice hash estático por generación del initrd. Guarda índices (no
// punteros) porque `nodes` puede realocarse mientras se analiza el tar.
// 0 significa casilla vacía; cada entrada almacena index + 1.
static size_t *node_hash_slots = NULL;
static size_t node_hash_capacity = 0;

// Índice por directorio: cada entrada apunta a un rango de hijos directos
// en dir_child_indices. El índice node_count representa la raíz sintética.
static size_t *dir_child_offsets = NULL;
static size_t *dir_child_indices = NULL;
static size_t dir_child_count = 0;

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

static uint64_t tarfs_name_hash(const char *name) {
  // FNV-1a 64-bit. La tabla usa capacidad potencia de dos.
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    hash ^= *p;
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Construye un índice compacto de hijos directos. Sin él, readdir(i) volvía
// a recorrer los N nodos del initrd para localizar el hijo i; getdents64
// sobre un directorio grande acababa costando O(N * hijos^2).
static int tarfs_build_dir_index(void) {
  if (node_count == 0 || !node_hash_slots)
    return -1;
  if (node_count > SIZE_MAX / sizeof(size_t) - 2)
    return -1;

  const size_t parent_count = node_count + 1; // nodos + raíz sintética
  size_t *counts = (size_t *)kzalloc(parent_count * sizeof(size_t));
  size_t *parents = (size_t *)kmalloc(node_count * sizeof(size_t));
  size_t *offsets = (size_t *)kzalloc((parent_count + 1) * sizeof(size_t));
  size_t *cursor = (size_t *)kmalloc(parent_count * sizeof(size_t));
  size_t *children = (size_t *)kmalloc(node_count * sizeof(size_t));
  if (!counts || !parents || !offsets || !cursor || !children) {
    if (counts)
      kfree(counts);
    if (parents)
      kfree(parents);
    if (offsets)
      kfree(offsets);
    if (cursor)
      kfree(cursor);
    if (children)
      kfree(children);
    return -1;
  }

  // Resolver el padre de cada ruta después de que nodes y el índice hash
  // estén completos. Exigimos directorios explícitos; si el tar no los
  // contiene, mantenemos el readdir antiguo para no alterar su semántica.
  for (size_t i = 0; i < node_count; i++) {
    const char *name = nodes[i].name;
    const char *last_slash = NULL;
    for (const char *p = name; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }

    size_t parent_index = node_count; // raíz sintética
    if (last_slash) {
      size_t parent_len = (size_t)(last_slash - name);
      if (parent_len == 0 || parent_len >= sizeof(nodes[0].name))
        goto fail;

      char parent_name[sizeof(nodes[0].name)];
      memcpy(parent_name, name, parent_len);
      parent_name[parent_len] = '\0';
      tar_node_t *parent = find_by_name(parent_name);
      if (!parent || !parent->is_dir)
        goto fail;
      parent_index = (size_t)(parent - nodes);
      if (parent_index >= node_count)
        goto fail;
    }

    parents[i] = parent_index;
    counts[parent_index]++;
  }

  offsets[0] = 0;
  for (size_t p = 0; p < parent_count; p++) {
    if (offsets[p] > SIZE_MAX - counts[p])
      goto fail;
    offsets[p + 1] = offsets[p] + counts[p];
  }
  memcpy(cursor, offsets, parent_count * sizeof(size_t));

  // Recorrer en orden de archive conserva el orden previo de readdir.
  for (size_t i = 0; i < node_count; i++)
    children[cursor[parents[i]]++] = i;

  if (dir_child_offsets)
    kfree(dir_child_offsets);
  if (dir_child_indices)
    kfree(dir_child_indices);
  dir_child_offsets = offsets;
  dir_child_indices = children;
  dir_child_count = offsets[parent_count];

  kfree(counts);
  kfree(parents);
  kfree(cursor);
  return 0;

fail:
  kfree(counts);
  kfree(parents);
  kfree(offsets);
  kfree(cursor);
  kfree(children);
  return -1;
}

static int tarfs_build_name_index(void) {
  if (node_count == 0)
    return 0;
  if (node_count > SIZE_MAX / 2)
    return -1;

  size_t required = node_count * 2;
  size_t capacity = 128;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2)
      return -1;
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(size_t))
    return -1;

  size_t *slots = (size_t *)kzalloc(capacity * sizeof(size_t));
  if (!slots)
    return -1;

  const size_t mask = capacity - 1;
  for (size_t i = 0; i < node_count; i++) {
    size_t slot = (size_t)tarfs_name_hash(nodes[i].name) & mask;
    while (slots[slot] != 0) {
      size_t existing = slots[slot] - 1;
      // Mantener la semántica anterior en caso de rutas duplicadas:
      // find_by_name devolvía la primera entrada del archivo tar.
      if (strcmp(nodes[existing].name, nodes[i].name) == 0)
        break;
      slot = (slot + 1) & mask;
    }
    if (slots[slot] == 0)
      slots[slot] = i + 1;
  }

  if (node_hash_slots)
    kfree(node_hash_slots);
  node_hash_slots = slots;
  node_hash_capacity = capacity;
  return 0;
}

static tar_node_t *find_by_name(const char *name) {
  if (!name)
    return NULL;

  if (node_hash_slots && node_hash_capacity != 0) {
    const size_t mask = node_hash_capacity - 1;
    size_t slot = (size_t)tarfs_name_hash(name) & mask;

    for (size_t probes = 0; probes < node_hash_capacity; probes++) {
      size_t encoded_index = node_hash_slots[slot];
      if (encoded_index == 0)
        return NULL;
      tar_node_t *node = &nodes[encoded_index - 1];
      if (strcmp(node->name, name) == 0)
        return node;
      slot = (slot + 1) & mask;
    }
    return NULL;
  }

  // Fallback correcto si no hay memoria para construir el índice.
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
  if (dir_child_offsets) {
    kfree(dir_child_offsets);
    dir_child_offsets = NULL;
  }
  if (dir_child_indices) {
    kfree(dir_child_indices);
    dir_child_indices = NULL;
  }
  dir_child_count = 0;
  if (node_hash_slots) {
    kfree(node_hash_slots);
    node_hash_slots = NULL;
  }
  node_hash_capacity = 0;
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
        LOG_PANIC(
            "[TARFS] No se pudo ampliar el índice de nodos (%lu entradas)",
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

  if (tarfs_build_name_index() != 0) {
    LOG_WARN("[TARFS] Sin memoria para el índice hash; usando búsqueda lineal");
  } else {
    LOG_INFO("[TARFS] Índice hash creado: %lu nodos, %lu casillas",
             (unsigned long)node_count, (unsigned long)node_hash_capacity);
    if (tarfs_build_dir_index() == 0) {
      LOG_INFO("[TARFS] Índice de directorios creado: %lu relaciones",
               (unsigned long)dir_child_count);
    } else {
      LOG_WARN(
          "[TARFS] Sin índice de directorios; readdir usa fallback lineal");
    }
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

  if (dir_child_offsets && dir_child_indices) {
    size_t dir_index =
        (tn == &g_tarfs_root) ? node_count : (size_t)(tn - nodes);
    if (dir_index <= node_count) {
      size_t start = dir_child_offsets[dir_index];
      size_t end = dir_child_offsets[dir_index + 1];
      if (index >= (uint64_t)(end - start)) {
        out->name[0] = '\0';
        out->type = 0;
        out->size = 0;
        return 0;
      }

      size_t child_index = dir_child_indices[start + (size_t)index];
      if (child_index < node_count) {
        tar_node_t *child = &nodes[child_index];
        const char *name = child->name;
        if (tn != &g_tarfs_root) {
          if (strncmp(name, tn->name, plen) == 0 && name[plen] == '/') {
            name += plen + 1;
          } else {
            // Índice inválido/inconsistente: usar abajo el recorrido fiable.
            goto readdir_slow;
          }
        }

        size_t rlen = strlen(name);
        if (rlen >= sizeof(out->name))
          rlen = sizeof(out->name) - 1;
        memcpy(out->name, name, rlen);
        out->name[rlen] = '\0';
        out->type = child->is_dir ? VFS_DIRECTORY : VFS_FILE;
        out->size = child->is_dir ? 0 : child->size;
        return 0;
      }
    }
  }

readdir_slow:;
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

// Construye un vfs_node_t desde un tar_node_t. Reutilizado por el
// lookup normal y por el caso raíz.
static vfs_node_t *tarfs_make_vfs_node(tar_node_t *tn) {
  vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;

  if (tn == &g_tarfs_root) {
    node->name[0] = '/';
    node->name[1] = '\0';
    node->inode = 0;
  } else {
    size_t nlen = strlen(tn->name);
    if (nlen >= sizeof(node->name))
      nlen = sizeof(node->name) - 1;
    memcpy(node->name, tn->name, nlen);
    node->name[nlen] = '\0';
    node->inode = (uint32_t)(tn - nodes) + 1;
  }

  node->flags = tn->is_dir ? VFS_DIRECTORY : VFS_FILE;
  node->size = tn->size;
  node->ops = tn->is_dir ? &tar_dir_ops : &tar_file_ops;
  node->priv = tn;

  if (tn->is_symlink) {
    node->is_symlink = 1;
    size_t tlen = strlen(tn->linkname);
    if (tlen >= sizeof(node->link_target))
      tlen = sizeof(node->link_target) - 1;
    memcpy(node->link_target, tn->linkname, tlen);
    node->link_target[tlen] = '\0';
    node->mode = S_IFLNK | 07777;
  } else if (tn->is_dir) {
    node->mode = S_IFDIR | (tn->mode & 07777);
  } else {
    node->mode = S_IFREG | (tn->mode & 07777);
  }
  node->uid = tn->uid;
  node->gid = tn->gid;
  return node;
}

// ===========================================================================
// fs lookup (vfs_fs_ops_t)
//
// Path RELATIVO al mount (empieza por '/'). Camina componente a
// componente resolviendo symlinks MID-PATH internamente. Si el
// symlink es el ÚLTIMO componente, lo devuelve con is_symlink=1 para
// que el VFS decida según el flag no_follow.
//
// Sin esto, un path como ".../perl/5.38/Data/Dumper.pm" donde 5.38 es
// un symlink a 5.38.2 fallaba: find_by_name() no encontraba el path
// completo porque el árbol solo tiene entradas reales bajo 5.38.2.
//
// [FIX] Al reconstruir `work` tras resolver un symlink mid-path, el
// resto del path (`p`) apunta DENTRO de `work`. Si copiáramos
// `resolved` sobre `work` directamente, corromperíamos los bytes que
// `rest` todavía no ha leído (solapamiento). Por eso copiamos el resto
// a un buffer temporal antes de tocar `work`.
// ===========================================================================
static vfs_node_t *tarfs_fs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;

  if (!path)
    return NULL;

  // Raíz.
  if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
    return tarfs_make_vfs_node(&g_tarfs_root);

  // Copia mutable del path sin '/' iniciales.
  char work[512];
  size_t wlen = 0;
  while (*path == '/')
    path++;
  while (*path && wlen < sizeof(work) - 1)
    work[wlen++] = *path++;
  work[wlen] = '\0';

  // Bucle de resolución: cada iteración intenta caminar `work`
  // entero. Si encuentra un symlink mid-path, lo resuelve, reescribe
  // `work` y vuelve a empezar. Si no, devuelve el nodo final.
  for (int depth = 0; depth < 16; depth++) {
    char current[256] = "";
    size_t cur_len = 0;
    int resolved_something = 0;
    const char *p = work;

    while (*p) {
      const char *seg = p;
      while (*p && *p != '/')
        p++;
      size_t seg_len = (size_t)(p - seg);
      int is_last = (*p == '\0');
      if (!is_last)
        p++; // consumir '/'

      if (seg_len == 0)
        continue;

      // candidate = current + "/" + seg
      char candidate[256];
      size_t clen = cur_len;
      if (clen > 0) {
        if (clen + 1 + seg_len >= sizeof(candidate))
          return NULL;
        memcpy(candidate, current, clen);
        candidate[clen++] = '/';
        memcpy(candidate + clen, seg, seg_len);
        clen += seg_len;
      } else {
        if (seg_len >= sizeof(candidate))
          return NULL;
        memcpy(candidate, seg, seg_len);
        clen = seg_len;
      }
      candidate[clen] = '\0';

      tar_node_t *tn = find_by_name(candidate);
      if (!tn)
        return NULL;

      if (tn->is_symlink) {
        if (is_last) {
          // Último componente: devolvemos el symlink tal cual para
          // que el VFS decida según su flag no_follow.
          return tarfs_make_vfs_node(tn);
        }

        // Mid-path: resolver internamente.
        const char *target = tn->linkname;
        char resolved[256];

        if (target[0] == '/') {
          collapse_path(target, resolved, sizeof(resolved));
        } else {
          char tmp[512];
          size_t tlen = strlen(target);
          if (cur_len > 0) {
            if (cur_len + 1 + tlen >= sizeof(tmp))
              return NULL;
            memcpy(tmp, current, cur_len);
            tmp[cur_len] = '/';
            memcpy(tmp + cur_len + 1, target, tlen + 1);
          } else {
            if (tlen >= sizeof(tmp))
              return NULL;
            memcpy(tmp, target, tlen + 1);
          }
          collapse_path(tmp, resolved, sizeof(resolved));
        }

        // Reconstruir work: resolved + "/" + resto (p).
        //
        // IMPORTANTE: `p` apunta DENTRO de `work`. Copiamos el resto
        // a un buffer temporal ANTES de sobreescribir `work` con
        // `resolved`, porque los rangos [0, rlen) y
        // [p_offset, p_offset+restlen) pueden solaparse (típicamente
        // cuando el symlink está cerca del final del path). Sin esto,
        // `memcpy(work, resolved, rlen)` corrompe los bytes que
        // `rest` todavía no ha leído, y la segunda copia mete basura
        // en `work`.
        char rest_buf[512];
        size_t restlen = strlen(p);
        if (restlen >= sizeof(rest_buf))
          return NULL;
        memcpy(rest_buf, p, restlen + 1);

        size_t rlen = strlen(resolved);
        if (rlen + 1 + restlen >= sizeof(work))
          return NULL;
        memcpy(work, resolved, rlen);
        if (restlen > 0) {
          work[rlen] = '/';
          memcpy(work + rlen + 1, rest_buf, restlen + 1);
        } else {
          work[rlen] = '\0';
        }
        resolved_something = 1;
        break;
      }

      // Componente regular: avanzar `current`.
      memcpy(current, candidate, clen + 1);
      cur_len = clen;
    }

    if (resolved_something)
      continue;

    // No hubo symlinks mid-path: `current` es el path final.
    tar_node_t *tn = find_by_name(current);
    if (!tn)
      return NULL;
    return tarfs_make_vfs_node(tn);
  }

  LOG_WARN("[TARFS] Symlink loop en '%s'", path);
  return NULL;
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