// tools/aurbuild.c
//
// Construye un .aur a partir de un ELF userland (ET_DYN o ET_EXEC).
// Embebe una sección PROGBITS|ALLOC sin nombre cuyo contenido es un
// struct aur_manifest seguido de los datos variables (strings, arrays).
//
// El loader de 4.2 la detecta por magic. El section header tiene
// sh_name = 0 porque .shstrtab está en medio del fichero y no queremos
// duplicarlo.
//
// Uso:
//   aurbuild [opciones] input.elf output.aur
//   aurbuild --info module.aur
//
// Formato de un .aur (resumen):
//   - Cabecera ELF ET_DYN/ET_EXEC
//   - Sección .aur al final (offset > todos los demás)
//   - Section header añadido al final de e_shoff
//
// Este fichero duplica struct aur_manifest y AUR_CAP_* de kernel/aur.h
// porque tools corre en el host y no puede incluir headers del kernel.
// Mantenerlos en sync.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#define AUR_MAGIC       0x41555221u
#define AUR_ABI_VERSION 1u
#define AUR_MAX_NAME    32
#define AUR_MAX_SERVICE 32

#define AUR_CAP_MMIO         (1u << 0)
#define AUR_CAP_DMA          (1u << 1)
#define AUR_CAP_IRQ          (1u << 2)
#define AUR_CAP_IPC_SERVICE  (1u << 3)
#define AUR_CAP_BUS_MATCH    (1u << 4)
#define AUR_CAP_BLOCK_DEVICE (1u << 5)
#define AUR_CAP_NETIF        (1u << 6)
#define AUR_CAP_USB_HCD      (1u << 7)

#define AUR_BUS_NONE 0
#define AUR_BUS_PCI  1
#define AUR_BUS_USB  2

typedef struct {
  uint8_t  bus;
  uint16_t vendor;
  uint16_t device;
  uint8_t  class_id;
  uint8_t  subclass_id;
  uint8_t  class_mask;
  uint8_t  _pad;
} aur_bus_match_t;

struct aur_manifest {
  uint32_t magic;
  uint16_t abi_version;
  uint16_t flags;
  uint32_t name_len;
  uint32_t name_off;
  uint32_t caps;
  uint32_t bus_match_count;
  uint32_t bus_match_off;
  uint32_t devfd_count;
  uint32_t devfd_off;
  uint32_t ipc_service_count;
  uint32_t ipc_service_off;
  uint32_t ring_per_cpu;
  uint32_t reserved[4];
};

#define MAX_BUS_MATCH 16
#define MAX_DEVFDS    8
#define MAX_IPC_SVC   8

typedef struct {
  aur_bus_match_t items[MAX_BUS_MATCH];
  int count;
  char devfds[MAX_DEVFDS][AUR_MAX_NAME];
  int devfd_count;
  char ipc_svcs[MAX_IPC_SVC][AUR_MAX_SERVICE];
  int ipc_count;
  char name[AUR_MAX_NAME];
  uint32_t caps;
  uint32_t ring_per_cpu;
} aur_args_t;

static void usage(const char *prog) {
  fprintf(stderr,
          "uso: %s [opciones] <input.elf> <output.aur>\n"
          "     %s --info <file.aur>\n"
          "\n"
          "opciones:\n"
          "  --name=<n>              nombre del módulo (default: basename del output)\n"
          "  --caps=<mask>           bitmask de AUR_CAP_* (default 0)\n"
          "  --pci-device=VVVV:DDDD  match PCI por vendor+device (class_mask=0)\n"
          "  --pci-class=CC:SS:MM    match PCI por class+subclass (mask=MM)\n"
          "  --devfd=<name>          declara /dev/aur/<name>/<devfd>\n"
          "  --ipc=<name>            declara servicio IPC\n"
          "  --ring-per-cpu=<N>      anillos por CPU (0 = 1)\n"
          "  --info                  muestra el manifiesto de un .aur existente\n",
          prog, prog);
}

static int parse_hex16(const char *s, uint16_t *out) {
  char *end;
  unsigned long v = strtoul(s, &end, 16);
  if (*end != '\0' || v > 0xFFFF)
    return -1;
  *out = (uint16_t)v;
  return 0;
}

static int parse_hex8(const char *s, uint8_t *out) {
  char *end;
  unsigned long v = strtoul(s, &end, 16);
  if (*end != '\0' || v > 0xFF)
    return -1;
  *out = (uint8_t)v;
  return 0;
}

static int parse_pci_device(const char *spec, aur_bus_match_t *out) {
  char buf[16];
  if (strlen(spec) >= sizeof(buf))
    return -1;
  strcpy(buf, spec);
  char *colon = strchr(buf, ':');
  if (!colon)
    return -1;
  *colon = '\0';
  uint16_t v, d;
  if (parse_hex16(buf, &v) != 0 || parse_hex16(colon + 1, &d) != 0)
    return -1;
  memset(out, 0, sizeof(*out));
  out->bus = AUR_BUS_PCI;
  out->vendor = v;
  out->device = d;
  out->class_mask = 0;
  return 0;
}

static int parse_pci_class(const char *spec, aur_bus_match_t *out) {
  char buf[24];
  if (strlen(spec) >= sizeof(buf))
    return -1;
  strcpy(buf, spec);
  char *p1 = strchr(buf, ':');
  if (!p1)
    return -1;
  *p1 = '\0';
  char *p2 = strchr(p1 + 1, ':');
  if (!p2)
    return -1;
  *p2 = '\0';
  uint8_t cc, ss, mm;
  if (parse_hex8(buf, &cc) != 0)
    return -1;
  if (parse_hex8(p1 + 1, &ss) != 0)
    return -1;
  if (parse_hex8(p2 + 1, &mm) != 0)
    return -1;
  memset(out, 0, sizeof(*out));
  out->bus = AUR_BUS_PCI;
  out->class_id = cc;
  out->subclass_id = ss;
  out->class_mask = mm;
  return 0;
}

static const char *basename_dup(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *b = slash ? slash + 1 : path;
  static char tmp[64];
  size_t n = strlen(b);
  if (n > 4 && strcmp(b + n - 4, ".aur") == 0)
    n -= 4;
  if (n >= sizeof(tmp))
    n = sizeof(tmp) - 1;
  memcpy(tmp, b, n);
  tmp[n] = '\0';
  return tmp;
}

static uint64_t align_up(uint64_t v, uint64_t a) {
  return (v + a - 1) & ~(a - 1);
}

// Busca la sección .aur por magic. Devuelve 0 si OK, -1 si no.
static int find_aur_section(const uint8_t *data, size_t size,
                            const Elf64_Shdr **out_shdr,
                            const struct aur_manifest **out_mf) {
  if (size < sizeof(Elf64_Ehdr))
    return -1;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
  if (memcmp(eh->e_ident, ELFMAG, 4) != 0)
    return -1;
  if (eh->e_shoff > size ||
      eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > size)
    return -1;
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(data + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_PROGBITS)
      continue;
    if (sh[i].sh_size < sizeof(struct aur_manifest))
      continue;
    if (sh[i].sh_offset + sh[i].sh_size > size)
      continue;
    const struct aur_manifest *mf =
        (const struct aur_manifest *)(data + sh[i].sh_offset);
    if (mf->magic == AUR_MAGIC && mf->abi_version == AUR_ABI_VERSION) {
      if (out_shdr)
        *out_shdr = &sh[i];
      if (out_mf)
        *out_mf = mf;
      return 0;
    }
  }
  return -1;
}

static int build_section_blob(const aur_args_t *a, uint8_t **out_data,
                              size_t *out_size) {
  size_t base = sizeof(struct aur_manifest);
  size_t name_off = base;
  size_t name_len = strlen(a->name) + 1;
  size_t off = align_up(name_off + name_len, 8);

  size_t bus_match_off = 0, devfd_off = 0, ipc_off = 0;
  if (a->count > 0) {
    bus_match_off = off;
    off = align_up(off + (size_t)a->count * sizeof(aur_bus_match_t), 8);
  }
  if (a->devfd_count > 0) {
    devfd_off = off;
    off = align_up(off + (size_t)a->devfd_count * AUR_MAX_NAME, 8);
  }
  if (a->ipc_count > 0) {
    ipc_off = off;
    off = align_up(off + (size_t)a->ipc_count * AUR_MAX_SERVICE, 8);
  }

  uint8_t *buf = calloc(1, off);
  if (!buf)
    return -1;
  struct aur_manifest *mf = (struct aur_manifest *)buf;
  mf->magic = AUR_MAGIC;
  mf->abi_version = AUR_ABI_VERSION;
  mf->flags = 0;
  mf->name_len = (uint32_t)strlen(a->name);
  mf->name_off = (uint32_t)name_off;
  mf->caps = a->caps;
  mf->bus_match_count = (uint32_t)a->count;
  mf->bus_match_off = (uint32_t)(a->count > 0 ? bus_match_off : 0);
  mf->devfd_count = (uint32_t)a->devfd_count;
  mf->devfd_off = (uint32_t)(a->devfd_count > 0 ? devfd_off : 0);
  mf->ipc_service_count = (uint32_t)a->ipc_count;
  mf->ipc_service_off = (uint32_t)(a->ipc_count > 0 ? ipc_off : 0);
  mf->ring_per_cpu = a->ring_per_cpu;
  memcpy(buf + name_off, a->name, name_len);
  if (a->count > 0)
    memcpy(buf + bus_match_off, a->items,
           (size_t)a->count * sizeof(aur_bus_match_t));
  if (a->devfd_count > 0)
    for (int i = 0; i < a->devfd_count; i++)
      memcpy(buf + devfd_off + (size_t)i * AUR_MAX_NAME, a->devfds[i],
             AUR_MAX_NAME);
  if (a->ipc_count > 0)
    for (int i = 0; i < a->ipc_count; i++)
      memcpy(buf + ipc_off + (size_t)i * AUR_MAX_SERVICE, a->ipc_svcs[i],
             AUR_MAX_SERVICE);

  *out_data = buf;
  *out_size = off;
  return 0;
}

static int cmd_info(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror("fopen");
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(f);
    return 1;
  }
  uint8_t *data = malloc((size_t)sz);
  if (!data) {
    fclose(f);
    return 1;
  }
  if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f);
    free(data);
    return 1;
  }
  fclose(f);

  const Elf64_Shdr *sh;
  const struct aur_manifest *mf;
  if (find_aur_section(data, (size_t)sz, &sh, &mf) != 0) {
    fprintf(stderr, "aurbuild: no hay sección .aur válida en '%s'\n", path);
    free(data);
    return 1;
  }

  if (mf->name_off + mf->name_len + 1 > sh->sh_size) {
    fprintf(stderr, "aurbuild: name_off fuera de rango\n");
    free(data);
    return 1;
  }
  const char *name = (const char *)(data + sh->sh_offset + mf->name_off);

  printf("aur module: %s\n", name);
  printf("  abi_version:  %u\n", mf->abi_version);
  printf("  caps:         0x%08x", mf->caps);
  if (mf->caps == 0) {
    printf(" (none)");
  } else {
    printf(" (");
    int first = 1;
#define CAP(x, s)                                                              \
  do {                                                                         \
    if (mf->caps & (x)) {                                                      \
      printf("%s%s", first ? "" : ", ", s);                                    \
      first = 0;                                                               \
    }                                                                          \
  } while (0)
    CAP(AUR_CAP_MMIO, "MMIO");
    CAP(AUR_CAP_DMA, "DMA");
    CAP(AUR_CAP_IRQ, "IRQ");
    CAP(AUR_CAP_IPC_SERVICE, "IPC");
    CAP(AUR_CAP_BUS_MATCH, "BUS");
    CAP(AUR_CAP_BLOCK_DEVICE, "BLK");
    CAP(AUR_CAP_NETIF, "NET");
    CAP(AUR_CAP_USB_HCD, "USB");
#undef CAP
    printf(")");
  }
  printf("\n");
  printf("  ring_per_cpu: %u\n", mf->ring_per_cpu);
  printf("  bus_match:    %u\n", mf->bus_match_count);
  if (mf->bus_match_count > 0) {
    const aur_bus_match_t *b = (const aur_bus_match_t *)(
        data + sh->sh_offset + mf->bus_match_off);
    for (uint32_t i = 0; i < mf->bus_match_count; i++)
      printf("    [%u] bus=%u vendor=%04x device=%04x class=%02x:%02x mask=%02x\n",
             i, b[i].bus, b[i].vendor, b[i].device,
             b[i].class_id, b[i].subclass_id, b[i].class_mask);
  }
  printf("  devfds:       %u\n", mf->devfd_count);
  if (mf->devfd_count > 0) {
    const char *base = (const char *)(data + sh->sh_offset + mf->devfd_off);
    for (uint32_t i = 0; i < mf->devfd_count; i++)
      printf("    [%u] %s\n", i, base + (size_t)i * AUR_MAX_NAME);
  }
  printf("  ipc_services: %u\n", mf->ipc_service_count);
  if (mf->ipc_service_count > 0) {
    const char *base = (const char *)(data + sh->sh_offset + mf->ipc_service_off);
    for (uint32_t i = 0; i < mf->ipc_service_count; i++)
      printf("    [%u] %s\n", i, base + (size_t)i * AUR_MAX_SERVICE);
  }

  free(data);
  return 0;
}

int main(int argc, char **argv) {
  aur_args_t args;
  memset(&args, 0, sizeof(args));
  int do_info = 0;
  const char *in = NULL, *out = NULL;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--info") == 0) {
      do_info = 1;
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else if (strncmp(a, "--name=", 7) == 0) {
      if (strlen(a + 7) >= AUR_MAX_NAME) {
        fprintf(stderr, "name demasiado largo\n");
        return 2;
      }
      strcpy(args.name, a + 7);
    } else if (strncmp(a, "--caps=", 7) == 0) {
      args.caps = (uint32_t)strtoul(a + 7, NULL, 0);
    } else if (strncmp(a, "--pci-device=", 13) == 0) {
      if (args.count >= MAX_BUS_MATCH) {
        fprintf(stderr, "MAX_BUS_MATCH\n");
        return 2;
      }
      if (parse_pci_device(a + 13, &args.items[args.count]) != 0) {
        fprintf(stderr, "pci-device inválido: %s\n", a + 13);
        return 2;
      }
      args.count++;
    } else if (strncmp(a, "--pci-class=", 12) == 0) {
      if (args.count >= MAX_BUS_MATCH) {
        fprintf(stderr, "MAX_BUS_MATCH\n");
        return 2;
      }
      if (parse_pci_class(a + 12, &args.items[args.count]) != 0) {
        fprintf(stderr, "pci-class inválido: %s\n", a + 12);
        return 2;
      }
      args.count++;
    } else if (strncmp(a, "--devfd=", 8) == 0) {
      if (args.devfd_count >= MAX_DEVFDS) {
        fprintf(stderr, "MAX_DEVFDS\n");
        return 2;
      }
      if (strlen(a + 8) >= AUR_MAX_NAME) {
        fprintf(stderr, "devfd demasiado largo\n");
        return 2;
      }
      strcpy(args.devfds[args.devfd_count++], a + 8);
    } else if (strncmp(a, "--ipc=", 6) == 0) {
      if (args.ipc_count >= MAX_IPC_SVC) {
        fprintf(stderr, "MAX_IPC_SVC\n");
        return 2;
      }
      if (strlen(a + 6) >= AUR_MAX_SERVICE) {
        fprintf(stderr, "ipc demasiado largo\n");
        return 2;
      }
      strcpy(args.ipc_svcs[args.ipc_count++], a + 6);
    } else if (strncmp(a, "--ring-per-cpu=", 15) == 0) {
      args.ring_per_cpu = (uint32_t)strtoul(a + 15, NULL, 0);
    } else if (a[0] == '-') {
      fprintf(stderr, "opción desconocida: %s\n", a);
      usage(argv[0]);
      return 2;
    } else if (!in) {
      in = a;
    } else if (!out) {
      out = a;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if (do_info) {
    if (!in) {
      usage(argv[0]);
      return 2;
    }
    return cmd_info(in);
  }
  if (!in || !out) {
    usage(argv[0]);
    return 2;
  }
  if (args.name[0] == '\0') {
    const char *default_name = basename_dup(out);
    size_t name_len = strlen(default_name);
    if (name_len >= sizeof(args.name)) {
      fprintf(stderr, "name demasiado largo: %s\n", default_name);
      return 2;
    }
    memcpy(args.name, default_name, name_len + 1);
  }

  // Leer el ELF
  FILE *f = fopen(in, "rb");
  if (!f) {
    perror("fopen(in)");
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(f);
    return 1;
  }
  uint8_t *data = malloc((size_t)sz);
  if (!data) {
    fclose(f);
    return 1;
  }
  if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f);
    free(data);
    return 1;
  }
  fclose(f);

  Elf64_Ehdr *eh = (Elf64_Ehdr *)data;
  if (memcmp(eh->e_ident, ELFMAG, 4) != 0) {
    fprintf(stderr, "aurbuild: no es ELF\n");
    free(data);
    return 1;
  }
  if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
      eh->e_ident[EI_DATA] != ELFDATA2LSB ||
      eh->e_machine != EM_X86_64) {
    fprintf(stderr, "aurbuild: no es ELF64 LSB x86-64\n");
    free(data);
    return 1;
  }
  if (eh->e_type != ET_DYN && eh->e_type != ET_EXEC) {
    fprintf(stderr, "aurbuild: e_type=%u, esperado ET_DYN(3) o ET_EXEC(2)\n",
            eh->e_type);
    free(data);
    return 1;
  }
  if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
      eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > (uint64_t)sz) {
    fprintf(stderr, "aurbuild: sin section headers válidos\n");
    free(data);
    return 1;
  }

  // Rechazar re-embebido
  {
    const Elf64_Shdr *dummy_sh;
    const struct aur_manifest *dummy_mf;
    if (find_aur_section(data, (size_t)sz, &dummy_sh, &dummy_mf) == 0) {
      fprintf(stderr,
              "aurbuild: '%s' ya tiene sección .aur; aurbuild no edita módulos "
              "existentes\n",
              in);
      free(data);
      return 1;
    }
  }

  // Construir la sección
  uint8_t *sec_data = NULL;
  size_t sec_size = 0;
  if (build_section_blob(&args, &sec_data, &sec_size) != 0) {
    fprintf(stderr, "aurbuild: sin memoria\n");
    free(data);
    return 1;
  }

  // Layout: original + section + new shdr table
  uint64_t sec_off = align_up((uint64_t)sz, 8);
  uint64_t new_shoff = align_up(sec_off + sec_size, 8);
  uint64_t new_size =
      new_shoff + ((uint64_t)eh->e_shnum + 1) * sizeof(Elf64_Shdr);

  uint8_t *outbuf = calloc(1, (size_t)new_size);
  if (!outbuf) {
    fprintf(stderr, "aurbuild: sin memoria para el output\n");
    free(data);
    free(sec_data);
    return 1;
  }

  memcpy(outbuf, data, (size_t)sz);
  memcpy(outbuf + sec_off, sec_data, sec_size);

  const Elf64_Shdr *orig_shdrs = (const Elf64_Shdr *)(data + eh->e_shoff);
  Elf64_Shdr *new_shdrs = (Elf64_Shdr *)(outbuf + new_shoff);
  memcpy(new_shdrs, orig_shdrs,
         (size_t)eh->e_shnum * sizeof(Elf64_Shdr));

  Elf64_Shdr *aur_sh = &new_shdrs[eh->e_shnum];
  memset(aur_sh, 0, sizeof(*aur_sh));
  aur_sh->sh_name = 0;
  aur_sh->sh_type = SHT_PROGBITS;
  aur_sh->sh_flags = SHF_ALLOC;
  aur_sh->sh_addr = 0;
  aur_sh->sh_offset = sec_off;
  aur_sh->sh_size = sec_size;
  aur_sh->sh_addralign = 8;

  Elf64_Ehdr *new_eh = (Elf64_Ehdr *)outbuf;
  new_eh->e_shoff = new_shoff;
  new_eh->e_shnum = (uint16_t)(eh->e_shnum + 1);

  FILE *o = fopen(out, "wb");
  if (!o) {
    perror("fopen(out)");
    free(data);
    free(sec_data);
    free(outbuf);
    return 1;
  }
  size_t wrote = fwrite(outbuf, 1, (size_t)new_size, o);
  fclose(o);
  free(data);
  free(sec_data);
  free(outbuf);
  if (wrote != (size_t)new_size)
    return 1;

  printf("aurbuild: %s -> %s\n", in, out);
  printf("  name:         %s\n", args.name);
  printf("  caps:         0x%08x\n", args.caps);
  printf("  bus_match:    %d\n", args.count);
  printf("  devfds:       %d\n", args.devfd_count);
  printf("  ipc_services: %d\n", args.ipc_count);
  printf("  size:         %ld bytes (ELF)\n", sz);
  printf("  .aur section: %lu bytes @ offset 0x%lx\n",
         (unsigned long)sec_size, (unsigned long)sec_off);
  return 0;
}