// kernel/sysfs.c
//
// sysfs: /sys, generado on-demand.
//
// Cobertura:
//   /sys/bus/pci/devices/                     dir
//   /sys/bus/pci/devices/<BDF>/               dir
//   /sys/bus/pci/devices/<BDF>/vendor         "0xVVVV\n"
//   /sys/bus/pci/devices/<BDF>/device         "0xDDDD\n"
//   /sys/bus/pci/devices/<BDF>/class          "0xCCSSPP\n"
//   /sys/bus/pci/devices/<BDF>/revision       "0xRR\n"
//   /sys/bus/pci/devices/<BDF>/subsystem_vendor "0xVVVV\n"
//   /sys/bus/pci/devices/<BDF>/subsystem_device "0xDDDD\n"
//   /sys/bus/pci/devices/<BDF>/config         256 bytes raw
//   /sys/bus/pci/devices/<BDF>/uevent         KEY=VALUE\n...
//   /sys/bus/pci/devices/<BDF>/irq            "0\n"
//   /sys/bus/pci/devices/<BDF>/enable         "1\n"
//   /sys/bus/pci/devices/<BDF>/modalias       "pci:v...d...sv...sd...bc..sc..i..\n"
//
//   /sys/dev/block/                           dir
//   /sys/dev/block/MM:mm                      symlink a /sys/class/block/NAME
//
//   /sys/class/block/                         dir
//   /sys/class/block/NAME/                    dir
//   /sys/class/block/NAME/dev                 "MM:mm\n"
//   /sys/class/block/NAME/size                "NNNN\n" (en unidades de 512 B)
//   /sys/class/block/NAME/ro                  "0\n" o "1\n"
//   /sys/class/block/NAME/removable           "0\n"
//
// El contenido se regenera en cada lookup (como procfs).
//
// BDF = "DDDD:BB:SS.F" donde D=domain, B=bus, S=slot, F=func.
// En 8-bit PCI (lo único que Aurora soporta hoy) domain siempre es 0.
//
// Nota sobre readdir de <BDF>:
//   BusyBox lspci hace recursive_action("/sys/bus/pci/devices") y para
//   cada entry X de <BDF> abre "<BDF>/X/uevent" y lo parsea como uevent
//   (KEY=VALUE). Si listamos todos los attrs (vendor, device, class...),
//   lspci abre "vendor/uevent" y recibe "0x8086\n" (1 token), y falla
//   con "bad line 1: 1 tokens found, 2 needed". Por eso readdir de <BDF>
//   expone SOLO "uevent". El resto de attrs siguen accesibles por lookup
//   normal (cat /sys/.../vendor sigue funcionando).

#include "sysfs.h"
#include "block.h"
#include "heap.h"
#include "io.h"        // inl / outl
#include "klog.h"
#include "string.h"
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

// ===========================================================================
// PCI config space access (puerto 0xCF8 / 0xCFC)
// ===========================================================================
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t off) {
  uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                  ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                  (uint32_t)(off & 0xFCu);
  outl(PCI_CONFIG_ADDR, addr);
  return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t off) {
  uint32_t v = pci_read_dword(bus, slot, func, off);
  return (uint16_t)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static uint8_t pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t off) {
  uint32_t v = pci_read_dword(bus, slot, func, off);
  return (uint8_t)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

// ===========================================================================
// Tabla de dispositivos PCI cacheada.
// ===========================================================================
#define SYSFS_PCI_MAX 64

typedef struct {
  uint8_t bus, slot, func;
  uint16_t vendor, device;
  uint8_t revision;
  uint8_t prog_if, subclass, class;
  uint16_t subsys_vendor, subsys_device;
} pci_dev_t;

static pci_dev_t g_pci[SYSFS_PCI_MAX];
static int g_pci_count = 0;
static int g_sysfs_ready = 0;

void sysfs_init(void) {
  if (g_sysfs_ready)
    return;
  g_sysfs_ready = 1;
  g_pci_count = 0;

  // Solo bus 0: QEMU/PIIX no pone nada en buses >0, y el escaneo
  // completo (256*32*8 = 65536 reads de config space) es lento y
  // completamente innecesario.
  for (int slot = 0; slot < 32; slot++) {
    uint16_t v0 = pci_read_word(0, slot, 0, 0x00);
    if (v0 == 0xFFFF)
      continue;
    uint8_t header = pci_read_byte(0, slot, 0, 0x0E);
    int nfuncs = (header & 0x80) ? 8 : 1;
    for (int func = 0; func < nfuncs; func++) {
      uint16_t vendor = pci_read_word(0, slot, func, 0x00);
      if (vendor == 0xFFFF)
        continue;
      if (g_pci_count >= SYSFS_PCI_MAX)
        goto done;
      pci_dev_t *d = &g_pci[g_pci_count++];
      d->bus = 0;
      d->slot = (uint8_t)slot;
      d->func = (uint8_t)func;
      d->vendor = vendor;
      d->device = pci_read_word(0, slot, func, 0x02);
      d->revision = pci_read_byte(0, slot, func, 0x08);
      d->prog_if = pci_read_byte(0, slot, func, 0x09);
      d->subclass = pci_read_byte(0, slot, func, 0x0A);
      d->class = pci_read_byte(0, slot, func, 0x0B);
      d->subsys_vendor = pci_read_word(0, slot, func, 0x2C);
      d->subsys_device = pci_read_word(0, slot, func, 0x2E);
    }
  }
done:
  LOG_INFO("[SYSFS] %d dispositivos PCI enumerados", g_pci_count);
}

// ===========================================================================
// Formato BDF: "0000:BB:SS.F"
// ===========================================================================
static void pci_bdf_str(const pci_dev_t *d, char *out, size_t outlen) {
  static const char hex[] = "0123456789abcdef";
  if (outlen < 13) {
    out[0] = '\0';
    return;
  }
  out[0] = '0';
  out[1] = '0';
  out[2] = '0';
  out[3] = '0';
  out[4] = ':';
  out[5] = hex[(d->bus >> 4) & 0xF];
  out[6] = hex[(d->bus) & 0xF];
  out[7] = ':';
  out[8] = hex[(d->slot >> 4) & 0xF];
  out[9] = hex[(d->slot) & 0xF];
  out[10] = '.';
  out[11] = (char)('0' + (d->func & 0xF));
  out[12] = '\0';
}

static int parse_pci_bdf(const char *s, uint8_t *bus, uint8_t *slot,
                         uint8_t *func) {
  if (!s)
    return -1;
  if (strlen(s) != 12)
    return -1;
  if (s[4] != ':' || s[7] != ':' || s[10] != '.')
    return -1;

  // domain: 4 hex
  for (int i = 0; i < 4; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return -1;
  }

  uint8_t b = 0, sl = 0;
  for (int i = 5; i < 7; i++) {
    char c = s[i];
    int v;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else
      return -1;
    b = (uint8_t)((b << 4) | v);
  }
  for (int i = 8; i < 10; i++) {
    char c = s[i];
    int v;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else
      return -1;
    sl = (uint8_t)((sl << 4) | v);
  }
  char fc = s[11];
  if (fc < '0' || fc > '9')
    return -1;

  *bus = b;
  *slot = sl;
  *func = (uint8_t)(fc - '0');
  return 0;
}

// ===========================================================================
// Helpers de formato
// ===========================================================================
static char *make_str_node_content(const char *s, size_t *out_len) {
  size_t n = strlen(s);
  char *buf = (char *)kmalloc(n + 1);
  if (!buf)
    return NULL;
  memcpy(buf, s, n + 1);
  *out_len = n;
  return buf;
}

static char *make_u64_content(uint64_t v, size_t *out_len) {
  char tmp[24];
  int n = 0;
  if (v == 0)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  char *buf = (char *)kmalloc((size_t)n + 2);
  if (!buf)
    return NULL;
  for (int i = 0; i < n; i++)
    buf[i] = tmp[n - 1 - i];
  buf[n] = '\n';
  buf[n + 1] = '\0';
  *out_len = (size_t)n + 1;
  return buf;
}

static char *make_hex_content(uint32_t v, int width, size_t *out_len) {
  static const char hex[] = "0123456789abcdef";
  char tmp[16];
  for (int i = width - 1; i >= 0; i--) {
    tmp[i] = hex[v & 0xF];
    v >>= 4;
  }
  size_t n = (size_t)width;
  // n + 4: "0x" + n dígitos + '\n' + '\0'
  char *buf = (char *)kmalloc(n + 4);
  if (!buf)
    return NULL;
  buf[0] = '0';
  buf[1] = 'x';
  memcpy(buf + 2, tmp, n);
  buf[n + 2] = '\n';
  buf[n + 3] = '\0';
  *out_len = n + 3;
  return buf;
}

// ===========================================================================
// Node ops
// ===========================================================================
typedef struct {
  char *buf;
  size_t len;
} sysfs_data_t;

static int64_t sysfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                          void *dst) {
  if (!node || !node->priv || !dst)
    return -EINVAL;
  sysfs_data_t *sd = (sysfs_data_t *)node->priv;
  if (offset >= sd->len)
    return 0;
  size_t n = sd->len - offset;
  if (n > size)
    n = size;
  memcpy(dst, sd->buf + offset, n);

  // [DIAG] Traza opcional. Descomenta si necesitas depurar.
  // LOG_INFO("[SYSFS-READ] node=%s off=%lu size=%lu -> n=%lu",
  //          node->name, (unsigned long)offset, (unsigned long)size,
  //          (unsigned long)n);

  return (int64_t)n;
}

static int sysfs_open(vfs_node_t *node, int flags) {
  (void)node;
  if ((flags & O_WRONLY) || (flags & O_RDWR))
    return -EROFS;
  return 0;
}

static int sysfs_close(vfs_node_t *node) {
  if (!node)
    return 0;
  sysfs_data_t *sd = (sysfs_data_t *)node->priv;
  if (sd) {
    if (sd->buf)
      kfree(sd->buf);
    kfree(sd);
    node->priv = NULL;
  }
  return 0;
}

static vfs_ops_t sysfs_file_ops = {
    .read = sysfs_read,
    .open = sysfs_open,
    .close = sysfs_close,
};

// Forward declaration del dispatcher de readdir.
static int sysfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out);

static vfs_ops_t sysfs_dir_ops = {
    .readdir = sysfs_readdir,
};

// ===========================================================================
// Construcción de nodos
// ===========================================================================
static vfs_node_t *make_dir(const char *name) {
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n)
    return NULL;
  size_t l = strlen(name);
  if (l >= sizeof(n->name))
    l = sizeof(n->name) - 1;
  memcpy(n->name, name, l);
  n->name[l] = '\0';
  n->flags = VFS_DIRECTORY;
  n->mode = S_IFDIR | 0555;
  n->uid = 0;
  n->gid = 0;
  n->ops = &sysfs_dir_ops;
  return n;
}

static vfs_node_t *make_file(const char *name, char *content, size_t len) {
  if (!content)
    return NULL;
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n) {
    kfree(content);
    return NULL;
  }
  sysfs_data_t *sd = (sysfs_data_t *)kzalloc(sizeof(*sd));
  if (!sd) {
    kfree(content);
    kfree(n);
    return NULL;
  }
  sd->buf = content;
  sd->len = len;

  size_t l = strlen(name);
  if (l >= sizeof(n->name))
    l = sizeof(n->name) - 1;
  memcpy(n->name, name, l);
  n->name[l] = '\0';
  n->flags = VFS_FILE;
  n->size = len;
  n->ops = &sysfs_file_ops;
  n->priv = sd;
  n->mode = S_IFREG | 0444;
  n->uid = 0;
  n->gid = 0;
  return n;
}

static vfs_node_t *make_symlink(const char *name, const char *target) {
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n)
    return NULL;
  size_t l = strlen(name);
  if (l >= sizeof(n->name))
    l = sizeof(n->name) - 1;
  memcpy(n->name, name, l);
  n->name[l] = '\0';
  size_t t = strlen(target);
  if (t >= sizeof(n->link_target))
    t = sizeof(n->link_target) - 1;
  memcpy(n->link_target, target, t);
  n->link_target[t] = '\0';
  n->is_symlink = 1;
  n->flags = VFS_FILE;
  n->mode = S_IFLNK | 0777;
  n->uid = 0;
  n->gid = 0;
  return n;
}

// ===========================================================================
// Listas estáticas para readdir
// ===========================================================================
static const char *sysfs_root_entries[] = {"bus", "class", "dev"};
#define SYSFS_N_ROOT \
  (sizeof(sysfs_root_entries) / sizeof(sysfs_root_entries[0]))

// [FIX] BusyBox lspci hace recursive_action("/sys/bus/pci/devices") y
// para cada entry X de <BDF> abre "<BDF>/X/uevent" y lo parsea como un
// uevent (KEY=VALUE). Solo X="uevent" da contenido válido. Si listamos
// todos los attrs, lspci abre vendor/uevent, recibe "0x8086\n" (1 token)
// y falla con "bad line 1: 1 tokens found, 2 needed".
//
// Los demás attrs siguen accesibles por lookup normal (cat sigue
// funcionando), simplemente no los exponemos en readdir para que lspci
// no los itere.
static const char *pci_dev_attrs[] = {"uevent"};
#define SYSFS_N_PCI_ATTRS \
  (sizeof(pci_dev_attrs) / sizeof(pci_dev_attrs[0]))

static const char *block_dev_attrs[] = {"dev", "size", "ro", "removable"};
#define SYSFS_N_BLK_ATTRS \
  (sizeof(block_dev_attrs) / sizeof(block_dev_attrs[0]))

// Sirve la entrada idx de una lista estática. 0 = servida (o EOF),
// out->name[0]=='\0' significa "no hay más".
static int readdir_list_typed(const char **list, size_t n, uint32_t type,
                              uint64_t idx, vfs_dirent_t *out) {
  if (idx >= n) {
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
    return 0;
  }
  size_t l = strlen(list[idx]);
  if (l >= sizeof(out->name))
    l = sizeof(out->name) - 1;
  memcpy(out->name, list[idx], l);
  out->name[l] = '\0';
  out->type = type;
  out->size = 0;
  return 0;
}

static int readdir_bdf_list(uint64_t idx, vfs_dirent_t *out) {
  if (idx >= (uint64_t)g_pci_count) {
    out->name[0] = '\0';
    return 1;
  }
  pci_bdf_str(&g_pci[idx], out->name, sizeof(out->name));
  out->type = VFS_DIRECTORY;
  out->size = 0;
  return 0;
}

static int readdir_block_names(uint64_t idx, vfs_dirent_t *out) {
  if (idx >= (uint64_t)blk_count()) {
    out->name[0] = '\0';
    return 1;
  }
  block_device_t *b = blk_get_by_index((int)idx);
  if (!b) {
    out->name[0] = '\0';
    return 1;
  }
  size_t l = strlen(b->name);
  if (l >= sizeof(out->name))
    l = sizeof(out->name) - 1;
  memcpy(out->name, b->name, l);
  out->name[l] = '\0';
  out->type = VFS_DIRECTORY;
  out->size = 0;
  return 0;
}

static int readdir_devblock(uint64_t idx, vfs_dirent_t *out) {
  if (idx >= (uint64_t)blk_count()) {
    out->name[0] = '\0';
    return 1;
  }
  // Formato "MM:mm". Usamos major=8 para todos los block devices y
  // minor=index. Es consistente con lo que devolvemos en
  // /sys/class/block/<name>/dev.
  uint32_t minor = (uint32_t)idx;
  int n = 0;
  char tmp[24];
  tmp[n++] = '8';
  tmp[n++] = ':';
  char mt[8];
  int mn = 0;
  if (minor == 0)
    mt[mn++] = '0';
  while (minor) {
    mt[mn++] = (char)('0' + minor % 10);
    minor /= 10;
  }
  while (mn > 0)
    tmp[n++] = mt[--mn];
  tmp[n] = '\0';
  memcpy(out->name, tmp, (size_t)n + 1);
  out->type = VFS_FILE;
  out->size = 0;
  return 0;
}

// ===========================================================================
// lookup
// ===========================================================================
static vfs_node_t *sysfs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;
  if (!path || path[0] != '/')
    return NULL;

  if (path[1] == '\0')
    return make_dir("/");

  const char *name = path + 1;

  // ---- /sys/bus ----
  if (strcmp(name, "bus") == 0)
    return make_dir("bus");
  if (strcmp(name, "bus/pci") == 0)
    return make_dir("pci");
  if (strcmp(name, "bus/pci/devices") == 0)
    return make_dir("devices");

  // ---- /sys/bus/pci/devices/<BDF>[/attr] ----
  if (strncmp(name, "bus/pci/devices/", 16) == 0) {
    const char *rest = name + 16;
    const char *slash = NULL;
    for (const char *p = rest; *p; p++) {
      if (*p == '/') {
        slash = p;
        break;
      }
    }
    char bdf[16];
    size_t bdf_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (bdf_len == 0 || bdf_len >= sizeof(bdf))
      return NULL;
    memcpy(bdf, rest, bdf_len);
    bdf[bdf_len] = '\0';

    uint8_t bus, slot, func;
    if (parse_pci_bdf(bdf, &bus, &slot, &func) != 0)
      return NULL;

    const pci_dev_t *d = NULL;
    for (int i = 0; i < g_pci_count; i++) {
      if (g_pci[i].bus == bus && g_pci[i].slot == slot &&
          g_pci[i].func == func) {
        d = &g_pci[i];
        break;
      }
    }
    if (!d)
      return NULL;

    if (!slash)
      return make_dir(bdf); // el directorio en sí

    const char *attr = slash + 1;

    // [FIX] Tolerar exactamente "<BDF>/uevent/uevent" como alias de
    // "<BDF>/uevent". Cualquier otro sufijo con "/" → NULL. Esto hace
    // que <BDF>/vendor/uevent, <BDF>/device/uevent, etc. NO existan,
    // que es lo correcto.
    char attr_buf[16];
    if (strcmp(attr, "uevent/uevent") == 0) {
      strcpy(attr_buf, "uevent");
      attr = attr_buf;
    } else {
      for (const char *p = attr; *p; p++)
        if (*p == '/')
          return NULL;
    }

    size_t len = 0;
    char *content = NULL;

    if (strcmp(attr, "vendor") == 0)
      content = make_hex_content(d->vendor, 4, &len);
    else if (strcmp(attr, "device") == 0)
      content = make_hex_content(d->device, 4, &len);
    else if (strcmp(attr, "class") == 0) {
      uint32_t cls = ((uint32_t)d->class << 16) |
                     ((uint32_t)d->subclass << 8) | (uint32_t)d->prog_if;
      content = make_hex_content(cls, 6, &len);
    } else if (strcmp(attr, "revision") == 0)
      content = make_hex_content(d->revision, 2, &len);
    else if (strcmp(attr, "subsystem_vendor") == 0)
      content = make_hex_content(d->subsys_vendor, 4, &len);
    else if (strcmp(attr, "subsystem_device") == 0)
      content = make_hex_content(d->subsys_device, 4, &len);
    else if (strcmp(attr, "config") == 0) {
      uint8_t *b = (uint8_t *)kmalloc(256);
      if (!b)
        return NULL;
      for (int off = 0; off < 256; off += 4) {
        uint32_t v = pci_read_dword(bus, slot, func, (uint8_t)off);
        b[off + 0] = (uint8_t)(v & 0xFF);
        b[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        b[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        b[off + 3] = (uint8_t)((v >> 24) & 0xFF);
      }
      len = 256;
      content = (char *)b;
    } else if (strcmp(attr, "irq") == 0) {
      content = make_str_node_content("0\n", &len);
    } else if (strcmp(attr, "enable") == 0) {
      content = make_str_node_content("1\n", &len);
    } else if (strcmp(attr, "modalias") == 0) {
      static const char hex[] = "0123456789abcdef";
      char tmp[96];
      int o = 0;
      const char *p = "pci:v0000";
      while (*p)
        tmp[o++] = *p++;
      tmp[o++] = hex[(d->vendor >> 12) & 0xF];
      tmp[o++] = hex[(d->vendor >> 8) & 0xF];
      tmp[o++] = hex[(d->vendor >> 4) & 0xF];
      tmp[o++] = hex[d->vendor & 0xF];
      tmp[o++] = 'd';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = hex[(d->device >> 12) & 0xF];
      tmp[o++] = hex[(d->device >> 8) & 0xF];
      tmp[o++] = hex[(d->device >> 4) & 0xF];
      tmp[o++] = hex[d->device & 0xF];
      tmp[o++] = 's';
      tmp[o++] = 'v';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = hex[(d->subsys_vendor >> 12) & 0xF];
      tmp[o++] = hex[(d->subsys_vendor >> 8) & 0xF];
      tmp[o++] = hex[(d->subsys_vendor >> 4) & 0xF];
      tmp[o++] = hex[d->subsys_vendor & 0xF];
      tmp[o++] = 's';
      tmp[o++] = 'd';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = hex[(d->subsys_device >> 12) & 0xF];
      tmp[o++] = hex[(d->subsys_device >> 8) & 0xF];
      tmp[o++] = hex[(d->subsys_device >> 4) & 0xF];
      tmp[o++] = hex[d->subsys_device & 0xF];
      tmp[o++] = 'b';
      tmp[o++] = 'c';
      tmp[o++] = hex[(d->class >> 4) & 0xF];
      tmp[o++] = hex[d->class & 0xF];
      tmp[o++] = 's';
      tmp[o++] = 'c';
      tmp[o++] = hex[(d->subclass >> 4) & 0xF];
      tmp[o++] = hex[d->subclass & 0xF];
      tmp[o++] = 'i';
      tmp[o++] = hex[(d->prog_if >> 4) & 0xF];
      tmp[o++] = hex[d->prog_if & 0xF];
      tmp[o++] = '\n';
      tmp[o] = '\0';

      char *buf = (char *)kmalloc((size_t)o + 1);
      if (!buf)
        return NULL;
      memcpy(buf, tmp, (size_t)o + 1);
      len = (size_t)o;
      content = buf;
    } else if (strcmp(attr, "uevent") == 0) {
      // /sys/bus/pci/devices/<BDF>/uevent — formato KEY=VALUE.
      // BusyBox lspci lo parsea con config_read(tokens, 3, 2, "\0:=").
      // El "2" significa: mínimo 2 tokens por línea. Cada línea debe
      // tener al menos "KEY=VAL".
      static const char hex[] = "0123456789abcdef";
      char tmp[256];
      int o = 0;
      const char *p;


      p = "PCI_CLASS=";
      while (*p)
        tmp[o++] = *p++;
      tmp[o++] = hex[(d->class >> 4) & 0xF];
      tmp[o++] = hex[d->class & 0xF];
      tmp[o++] = hex[(d->subclass >> 4) & 0xF];
      tmp[o++] = hex[d->subclass & 0xF];
      tmp[o++] = hex[(d->prog_if >> 4) & 0xF];
      tmp[o++] = hex[d->prog_if & 0xF];
      tmp[o++] = '\n';

      p = "PCI_ID=";
      while (*p)
        tmp[o++] = *p++;
      tmp[o++] = hex[(d->vendor >> 12) & 0xF];
      tmp[o++] = hex[(d->vendor >> 8) & 0xF];
      tmp[o++] = hex[(d->vendor >> 4) & 0xF];
      tmp[o++] = hex[d->vendor & 0xF];
      tmp[o++] = ':';
      tmp[o++] = hex[(d->device >> 12) & 0xF];
      tmp[o++] = hex[(d->device >> 8) & 0xF];
      tmp[o++] = hex[(d->device >> 4) & 0xF];
      tmp[o++] = hex[d->device & 0xF];
      tmp[o++] = '\n';

      p = "PCI_SUBSYS_ID=";
      while (*p)
        tmp[o++] = *p++;
      tmp[o++] = hex[(d->subsys_vendor >> 12) & 0xF];
      tmp[o++] = hex[(d->subsys_vendor >> 8) & 0xF];
      tmp[o++] = hex[(d->subsys_vendor >> 4) & 0xF];
      tmp[o++] = hex[d->subsys_vendor & 0xF];
      tmp[o++] = ':';
      tmp[o++] = hex[(d->subsys_device >> 12) & 0xF];
      tmp[o++] = hex[(d->subsys_device >> 8) & 0xF];
      tmp[o++] = hex[(d->subsys_device >> 4) & 0xF];
      tmp[o++] = hex[d->subsys_device & 0xF];
      tmp[o++] = '\n';

      p = "PCI_SLOT_NAME=";
      while (*p)
        tmp[o++] = *p++;
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = '0';
      tmp[o++] = ':';
      tmp[o++] = hex[(d->bus >> 4) & 0xF];
      tmp[o++] = hex[d->bus & 0xF];
      tmp[o++] = ':';
      tmp[o++] = hex[(d->slot >> 4) & 0xF];
      tmp[o++] = hex[d->slot & 0xF];
      tmp[o++] = '.';
      tmp[o++] = (char)('0' + (d->func & 0x7));
      tmp[o++] = '\n';

      tmp[o] = '\0';

      char *buf = (char *)kmalloc((size_t)o + 1);
      if (!buf)
        return NULL;
      memcpy(buf, tmp, (size_t)o + 1);
      len = (size_t)o;
      content = buf;
    } else {
      return NULL;
    }
    return make_file(attr, content, len);
  }

  // ---- /sys/class ----
  if (strcmp(name, "class") == 0)
    return make_dir("class");
  if (strcmp(name, "class/block") == 0)
    return make_dir("block");

  // ---- /sys/class/block/<name>[/attr] ----
  if (strncmp(name, "class/block/", 12) == 0) {
    const char *rest = name + 12;
    const char *slash = NULL;
    for (const char *p = rest; *p; p++) {
      if (*p == '/') {
        slash = p;
        break;
      }
    }
    char bname[16];
    size_t bl = slash ? (size_t)(slash - rest) : strlen(rest);
    if (bl == 0 || bl >= sizeof(bname))
      return NULL;
    memcpy(bname, rest, bl);
    bname[bl] = '\0';

    block_device_t *b = blk_lookup(bname);
    if (!b)
      return NULL;

    if (!slash)
      return make_dir(bname);

    const char *attr = slash + 1;
    for (const char *p = attr; *p; p++)
      if (*p == '/')
        return NULL;

    // Índice del dispositivo → minor.
    int idx = -1;
    int n = blk_count();
    for (int i = 0; i < n; i++) {
      if (blk_get_by_index(i) == b) {
        idx = i;
        break;
      }
    }
    if (idx < 0)
      return NULL;

    size_t len = 0;
    char *content = NULL;

    if (strcmp(attr, "dev") == 0) {
      char tmp[24];
      int o = 0;
      tmp[o++] = '8';
      tmp[o++] = ':';
      int v = idx;
      char mt[8];
      int mn = 0;
      if (v == 0)
        mt[mn++] = '0';
      while (v) {
        mt[mn++] = (char)('0' + v % 10);
        v /= 10;
      }
      while (mn > 0)
        tmp[o++] = mt[--mn];
      tmp[o++] = '\n';
      tmp[o] = '\0';
      content = make_str_node_content(tmp, &len);
    } else if (strcmp(attr, "size") == 0) {
      // En unidades de 512 B (como Linux).
      uint64_t sz512 = b->num_sectors * ((uint64_t)b->sector_size / 512);
      content = make_u64_content(sz512, &len);
    } else if (strcmp(attr, "ro") == 0) {
      content = make_str_node_content(b->is_read_only ? "1\n" : "0\n", &len);
    } else if (strcmp(attr, "removable") == 0) {
      content = make_str_node_content("0\n", &len);
    } else {
      return NULL;
    }
    return make_file(attr, content, len);
  }

  // ---- /sys/dev ----
  if (strcmp(name, "dev") == 0)
    return make_dir("dev");
  if (strcmp(name, "dev/block") == 0)
    return make_dir("block");

  // ---- /sys/dev/block/MM:mm → symlink a /sys/class/block/<name> ----
  if (strncmp(name, "dev/block/", 10) == 0) {
    const char *rest = name + 10;
    // Formato "8:N"
    if (rest[0] != '8' || rest[1] != ':')
      return NULL;
    const char *num = rest + 2;
    int minor = 0;
    while (*num >= '0' && *num <= '9') {
      minor = minor * 10 + (*num - '0');
      num++;
    }
    if (*num != '\0')
      return NULL;
    block_device_t *b = blk_get_by_index(minor);
    if (!b)
      return NULL;

    char target[64];
    int o = 0;
    const char *pfx = "/sys/class/block/";
    while (*pfx && o + 1 < (int)sizeof(target))
      target[o++] = *pfx++;
    size_t nl = strlen(b->name);
    for (size_t i = 0; i < nl && o + 1 < (int)sizeof(target); i++)
      target[o++] = b->name[i];
    target[o] = '\0';

    char link_name[16];
    int j = 0;
    link_name[j++] = '8';
    link_name[j++] = ':';
    int v = minor;
    char mt[8];
    int mn = 0;
    if (v == 0)
      mt[mn++] = '0';
    while (v) {
      mt[mn++] = (char)('0' + v % 10);
      v /= 10;
    }
    while (mn > 0)
      link_name[j++] = mt[--mn];
    link_name[j] = '\0';

    return make_symlink(link_name, target);
  }

  return NULL;
}

// ===========================================================================
// readdir dispatcher
// ===========================================================================
static int sysfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  if (!dir || !out)
    return -EINVAL;
  if (!(dir->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  // `dir->name` es el path COMPLETO tal como lo ve el usuario
  // ("/sys", "/sys/bus", "/sys/bus/pci", ...), no el path relativo al
  // mount. sysfs_lookup() recibe paths relativos ("bus", "class"), así
  // que aquí hay que quitar el prefijo "/sys" para poder comparar con
  // las mismas claves.
  const char *n = dir->name;
  if (strncmp(n, "/sys", 4) == 0) {
    n += 4;
    if (*n == '\0')
      n = "/";
  }

  // A partir de aquí `n` es "/", "/bus", "/bus/pci", "/bus/pci/devices",
  // "/bus/pci/devices/<BDF>", "/class", "/class/block",
  // "/class/block/<name>", "/dev", "/dev/block".

  if (strcmp(n, "/") == 0)
    return readdir_list_typed(sysfs_root_entries, SYSFS_N_ROOT,
                              VFS_DIRECTORY, index, out);

  if (strcmp(n, "/bus") == 0) {
    static const char *l[] = {"pci"};
    return readdir_list_typed(l, 1, VFS_DIRECTORY, index, out);
  }
  if (strcmp(n, "/bus/pci") == 0) {
    static const char *l[] = {"devices"};
    return readdir_list_typed(l, 1, VFS_DIRECTORY, index, out);
  }
  if (strcmp(n, "/bus/pci/devices") == 0) {
    if (readdir_bdf_list(index, out) == 1) {
      out->name[0] = '\0';
      out->type = 0;
      out->size = 0;
    }
    return 0;
  }
  if (strncmp(n, "/bus/pci/devices/", 17) == 0) {
    const char *rest = n + 17;
    for (const char *p = rest; *p; p++)
      if (*p == '/')
        return 0;
    return readdir_list_typed(pci_dev_attrs, SYSFS_N_PCI_ATTRS,
                              VFS_FILE, index, out);
  }

  if (strcmp(n, "/class") == 0) {
    static const char *l[] = {"block"};
    return readdir_list_typed(l, 1, VFS_DIRECTORY, index, out);
  }
  if (strcmp(n, "/class/block") == 0) {
    if (readdir_block_names(index, out) == 1) {
      out->name[0] = '\0';
      out->type = 0;
      out->size = 0;
    }
    return 0;
  }
  if (strncmp(n, "/class/block/", 13) == 0) {
    const char *rest = n + 13;
    for (const char *p = rest; *p; p++)
      if (*p == '/')
        return 0;
    return readdir_list_typed(block_dev_attrs, SYSFS_N_BLK_ATTRS,
                              VFS_FILE, index, out);
  }

  if (strcmp(n, "/dev") == 0) {
    static const char *l[] = {"block"};
    return readdir_list_typed(l, 1, VFS_DIRECTORY, index, out);
  }
  if (strcmp(n, "/dev/block") == 0) {
    if (readdir_devblock(index, out) == 1) {
      out->name[0] = '\0';
      out->type = 0;
      out->size = 0;
    }
    return 0;
  }

  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;
  return 0;
}

// ===========================================================================
// fs_ops
// ===========================================================================
static int sysfs_statfs(void *fs_priv, struct vfs_statfs *out) {
  (void)fs_priv;
  memset(out, 0, sizeof(*out));
  out->f_type = 0x62656572; // "sysfs" magic
  out->f_bsize = 4096;
  out->f_frsize = 4096;
  out->f_namelen = 255;
  return 0;
}

static vfs_fs_ops_t sysfs_fs_ops = {
    .lookup = sysfs_lookup,
    .statfs = sysfs_statfs,
    .name = "sysfs",
};

struct vfs_fs_ops *sysfs_get_vfs_ops(void) {
  return (struct vfs_fs_ops *)&sysfs_fs_ops;
}

// ---------------------------------------------------------------------------
// API pública para procfs y para /proc/bus/pci.
// ---------------------------------------------------------------------------
int sysfs_pci_count(void) { return g_pci_count; }

const struct sysfs_pci_info *sysfs_pci_get(int i) {
  if (i < 0 || i >= g_pci_count)
    return NULL;
  // Mismo layout: el struct local y el expuesto tienen los mismos
  // campos en el mismo orden (ambos packed por GCC en el mismo ABI).
  return (const struct sysfs_pci_info *)&g_pci[i];
}

// [FIX] Lee config space (256 bytes) de un dispositivo ya enumerado.
// La usa procfs para servir /proc/bus/pci/<bus>/<devfn>.
int sysfs_pci_read_config(uint8_t bus, uint8_t slot, uint8_t func,
                          uint8_t out[256]) {
  if (!out)
    return -EINVAL;

  const pci_dev_t *d = NULL;
  for (int i = 0; i < g_pci_count; i++) {
    if (g_pci[i].bus == bus && g_pci[i].slot == slot &&
        g_pci[i].func == func) {
      d = &g_pci[i];
      break;
    }
  }
  if (!d)
    return -ENOENT;

  for (int off = 0; off < 256; off += 4) {
    uint32_t v = pci_read_dword(bus, slot, func, (uint8_t)off);
    out[off + 0] = (uint8_t)(v & 0xFF);
    out[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    out[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    out[off + 3] = (uint8_t)((v >> 24) & 0xFF);
  }
  return 0;
}