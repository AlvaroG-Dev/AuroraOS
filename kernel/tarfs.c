// kernel/tarfs.c
#include "tarfs.h"
#include "klog.h"
#include "serial.h"
#include "string.h"

// Estructura oficial de cabecera USTAR (512 bytes)
typedef struct {
  char filename[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12]; // Tamaño en octal ASCII
  char mtime[12];
  char chksum[8];
  char typeflag; // '0' / '\0' = Archivo, '5' = Directorio
  char linkname[100];
  char magic[6]; // "ustar"
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char pad[12];
} __attribute__((packed)) ustar_header_t;

#define MAX_NODES 256

static tar_node_t nodes[MAX_NODES];
static size_t node_count = 0;

// ---------------------------------------------------------------------------
// Helpers internos de cadena
// ---------------------------------------------------------------------------

// Convierte octal ASCII a entero de 64 bits
static size_t parse_octal(const char *str, size_t max_len) {
  size_t n = 0;
  for (size_t i = 0; i < max_len && str[i] >= '0' && str[i] <= '7'; i++) {
    n = (n << 3) + (str[i] - '0');
  }
  return n;
}

// Omite prefijos ./ o / para normalizar rutas
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

// ---------------------------------------------------------------------------
// API TarFS
// ---------------------------------------------------------------------------
void tarfs_init(const void *tar_addr, size_t tar_size) {
  node_count = 0;
  uint8_t *ptr = (uint8_t *)tar_addr;
  uint8_t *end = ptr + tar_size;
  int nodes_full = 0;

  LOG_INFO("[TARFS] Analizando Initramfs en %p (%lu bytes)...", tar_addr,
           (unsigned long)tar_size);

  while (ptr + 512 <= end) {
    ustar_header_t *hdr = (ustar_header_t *)ptr;

    // Fin del archivo TAR (bloque de ceros)
    if (hdr->filename[0] == '\0') {
      break;
    }

    // Verificar magia USTAR
    if (strncmp(hdr->magic, "ustar", 5) != 0) {
      break;
    }

    size_t file_size = parse_octal(hdr->size, sizeof(hdr->size));
    int is_dir = (hdr->typeflag == '5');

    // Reconstruir la ruta respetando el campo 'prefix' de USTAR
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
          LOG_WARN("[TARFS] MAX_NODES (%d) alcanzado. Ignorando el resto "
                   "del initramfs. Aumenta MAX_NODES en tarfs.c.",
                   MAX_NODES);
          nodes_full = 1;
        }
        // No rompemos: seguimos avanzando `ptr` para llegar al final
        // limpiamente y poder reportar "20 nodos registrados" en vez
        // de dejar el bucle a medias.
      } else {
        tar_node_t *node = &nodes[node_count++];

        // Copia segura de la ruta.
        size_t cp_len = strlen(clean_path);
        if (cp_len >= sizeof(node->name)) {
          cp_len = sizeof(node->name) - 1;
        }
        for (size_t i = 0; i < cp_len; i++)
          node->name[i] = clean_path[i];
        node->name[cp_len] = '\0';

        // Quitar barra final de directorios para búsquedas homogéneas
        size_t nlen = strlen(node->name);
        if (nlen > 0 && node->name[nlen - 1] == '/') {
          node->name[nlen - 1] = '\0';
          is_dir = 1;
        }

        node->data = ptr + 512;
        node->size = file_size;
        node->is_dir = is_dir;

        if (!is_dir) {
          LOG_INFO("  [TARFS] <FILE> %s (%lu bytes)", node->name,
                   (unsigned long)file_size);
        } else {
          LOG_INFO("  [TARFS] <DIR>  %s", node->name);
        }
      }
    }

    // Avanzar el puntero: Cabecera (512B) + Bloques alineados a 512B de datos
    size_t data_blocks = (file_size + 511) / 512;
    ptr += 512 + (data_blocks * 512);
  }

  LOG_INFO("[TARFS] Carga completa. %lu nodos registrados.",
           (unsigned long)node_count);
}

tar_node_t *tarfs_open(const char *path) {
  const char *target = normalize_path(path);

  char clean_target[256];
  strcpy(clean_target, target);
  size_t len = strlen(clean_target);
  if (len > 0 && clean_target[len - 1] == '/') {
    clean_target[len - 1] = '\0';
  }

  for (size_t i = 0; i < node_count; i++) {
    if (strcmp(nodes[i].name, clean_target) == 0) {
      return &nodes[i];
    }
  }
  return NULL;
}

void tarfs_list(const char *dir_path) {
  const char *target = normalize_path(dir_path);
  size_t tlen = strlen(target);

  LOG_INFO("[TARFS] Listando directorio: '/%s':", target);

  for (size_t i = 0; i < node_count; i++) {
    if (tlen == 0 ||
        (strncmp(nodes[i].name, target, tlen) == 0 &&
         (nodes[i].name[tlen] == '/' || nodes[i].name[tlen] == '\0'))) {
      LOG_INFO("  - %s%s", nodes[i].is_dir ? "[DIR]  " : "[FILE] ",
               nodes[i].name);
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
