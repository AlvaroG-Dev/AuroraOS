// kernel/tarfs.c
#include "tarfs.h"
#include "klog.h"
#include "serial.h"
#include "string.h"

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

#define MAX_NODES 512

static tar_node_t nodes[MAX_NODES];
static size_t node_count = 0;

static size_t parse_octal(const char *str, size_t max_len) {
  size_t n = 0;
  for (size_t i = 0; i < max_len && str[i] >= '0' && str[i] <= '7'; i++)
    n = (n << 3) + (str[i] - '0');
  return n;
}

static const char *normalize_path(const char *path) {
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

// Colapsa //, elimina ".", resuelve ".." sin pasar de raíz. out nunca
// empieza por '/'. Si el path queda vacío, out = "".
static void collapse_path(const char *in, char *out, size_t outlen) {
  if (!in || !out || outlen < 2) {
    if (outlen)
      out[0] = '\0';
    return;
  }

  // Split en segmentos manualmente.
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
      // Subir: quitar el último segmento de out.
      while (o > 0 && out[o - 1] != '/')
        o--;
      if (o > 0)
        o--; // quitar la barra
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

void tarfs_init(const void *tar_addr, size_t tar_size) {
  node_count = 0;
  uint8_t *ptr = (uint8_t *)tar_addr;
  uint8_t *end = ptr + tar_size;
  int nodes_full = 0;

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

    const char *clean_path = normalize_path(full_path);

    if (clean_path[0] != '\0') {
      if (node_count >= MAX_NODES) {
        if (!nodes_full) {
          LOG_WARN("[TARFS] MAX_NODES (%d) alcanzado.", MAX_NODES);
          nodes_full = 1;
        }
      } else {
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

// Resuelve symlinks siguiendo el target hasta un nodo no-symlink.
// Detiene tras 8 saltos para evitar ciclos.
tar_node_t *tarfs_open(const char *path) {
  char current[256];
  const char *norm = normalize_path(path);
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
      // Absoluto. Se interpreta contra la raíz del tar.
      collapse_path(target, next, sizeof(next));
    } else {
      // Relativo al directorio del symlink.
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
    // Solo llegamos aquí si target[0]=='/':
    strncpy(current, next, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';
  }

  LOG_WARN("[TARFS] Symlink loop en '%s'", path);
  return NULL;
}

void tarfs_list(const char *dir_path) {
  const char *target = normalize_path(dir_path);
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