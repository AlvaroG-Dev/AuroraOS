// kernel/tarfs.c
#include "tarfs.h"
#include "serial.h"

// Estructura oficial de cabecera USTAR (512 bytes)
typedef struct {
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];       // Tamaño en octal ASCII
    char mtime[12];
    char chksum[8];
    char typeflag;       // '0' / '\0' = Archivo, '5' = Directorio
    char linkname[100];
    char magic[6];       // "ustar"
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
static size_t kstrlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static int kstrncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static void kstrcpy(char *dest, const char *src) {
    while ((*dest++ = *src++));
}

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

    serial_puts("[TARFS] Analizando Initramfs en 0x");
    serial_hex((uint64_t)tar_addr);
    serial_puts(" (");
    serial_putn(tar_size, 10, 0);
    serial_puts(" bytes)...\n");

    while (ptr + 512 <= end) {
        ustar_header_t *hdr = (ustar_header_t *)ptr;

        // Fin del archivo TAR (bloque de ceros)
        if (hdr->filename[0] == '\0') {
            break;
        }

        // Verificar magia USTAR
        if (kstrncmp(hdr->magic, "ustar", 5) != 0) {
            break;
        }

        size_t file_size = parse_octal(hdr->size, sizeof(hdr->size));
        int is_dir = (hdr->typeflag == '5');

        // Reconstruir la ruta respetando el campo 'prefix' de USTAR
        char full_path[256];
        size_t pos = 0;

        if (hdr->prefix[0] != '\0') {
            size_t plen = kstrlen(hdr->prefix);
            for (size_t i = 0; i < plen && pos < 255; i++) full_path[pos++] = hdr->prefix[i];
            if (pos < 255) full_path[pos++] = '/';
        }

        size_t fnlen = kstrlen(hdr->filename);
        for (size_t i = 0; i < fnlen && pos < 255; i++) full_path[pos++] = hdr->filename[i];
        full_path[pos] = '\0';

        const char *clean_path = normalize_path(full_path);

        if (clean_path[0] != '\0' && node_count < MAX_NODES) {
            tar_node_t *node = &nodes[node_count++];
            kstrcpy(node->name, clean_path);

            // Quitar barra final de directorios para búsquedas homogéneas
            size_t nlen = kstrlen(node->name);
            if (nlen > 0 && node->name[nlen - 1] == '/') {
                node->name[nlen - 1] = '\0';
                is_dir = 1;
            }

            node->data = ptr + 512;
            node->size = file_size;
            node->is_dir = is_dir;

            serial_puts("  [TARFS] ");
            serial_puts(is_dir ? "<DIR>  " : "<FILE> ");
            serial_puts(node->name);
            if (!is_dir) {
                serial_puts(" (");
                serial_putn(file_size, 10, 0);
                serial_puts(" bytes)");
            }
            serial_puts("\n");
        }

        // Avanzar el puntero: Cabecera (512B) + Bloques alineados a 512B de datos
        size_t data_blocks = (file_size + 511) / 512;
        ptr += 512 + (data_blocks * 512);
    }

    serial_puts("[TARFS] Carga completa. ");
    serial_putn(node_count, 10, 0);
    serial_puts(" nodos registrados.\n");
}

tar_node_t *tarfs_open(const char *path) {
    const char *target = normalize_path(path);

    char clean_target[256];
    kstrcpy(clean_target, target);
    size_t len = kstrlen(clean_target);
    if (len > 0 && clean_target[len - 1] == '/') {
        clean_target[len - 1] = '\0';
    }

    for (size_t i = 0; i < node_count; i++) {
        if (kstrcmp(nodes[i].name, clean_target) == 0) {
            return &nodes[i];
        }
    }
    return NULL;
}

void tarfs_list(const char *dir_path) {
    const char *target = normalize_path(dir_path);
    size_t tlen = kstrlen(target);

    serial_puts("[TARFS] Listando directorio: '/");
    serial_puts(target);
    serial_puts("':\n");

    for (size_t i = 0; i < node_count; i++) {
        if (tlen == 0 || (kstrncmp(nodes[i].name, target, tlen) == 0 &&
            (nodes[i].name[tlen] == '/' || nodes[i].name[tlen] == '\0'))) {
            serial_puts("  - ");
            serial_puts(nodes[i].is_dir ? "[DIR]  " : "[FILE] ");
            serial_puts(nodes[i].name);
            serial_puts("\n");
        }
    }
}

size_t tarfs_get_node_count(void) {
    return node_count;
}

tar_node_t *tarfs_get_node(size_t index) {
    if (index >= node_count) return NULL;
    return &nodes[index];
}

tar_node_t *tar_find_file(const char *path) {
    return tarfs_open(path);
}