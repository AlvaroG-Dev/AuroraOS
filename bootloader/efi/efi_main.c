// bootloader/efi_main.c
// UEFI Bootloader para Aurora OS — versión sin gnu-efi.
//
// Layout del medio (mismo que Windows/Linux):
//   efi.img (FAT16) contiene:
//     /EFI/BOOT/BOOTX64.EFI   → este binario
//     /aurora/kernel.elf      → el kernel (ELF64, ~2-3 MB)
//     /aurora/initrd.tar      → initramfs (~200+ MB)
//     /aurora/aurora.conf     → config
//   La ISO9660 solo contiene efi.img. NO dependemos de que el firmware
//   sepa leer ISO9660: solo FAT, garantizado por UEFI.

#include "../include/kernel_boot_info.h"
#include "efi_min.h"
#include "splash.h"
#include <stdarg.h>

/* ---- Globals ---- */
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_HANDLE gImageHandle = NULL;

/* ---- GUIDs ---- */
const EFI_GUID gEfiLoadedImageProtocolGuid = {
    0x5B1B31A1,
    0x9562,
    0x11D2,
    {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
const EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x964E5B22,
    0x6459,
    0x11D2,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
const EFI_GUID gEfiGraphicsOutputProtocolGuid = {
    0x9042A9DE,
    0x23DC,
    0x4A38,
    {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
/* EFI_EDID_ACTIVE_PROTOCOL / EFI_EDID_DISCOVERED_PROTOCOL */
static const EFI_GUID gEfiEdidActiveGuid = {
    0xBD8C1056, 0x9F36, 0x44EC, {0x92, 0xA8, 0xA6, 0x33, 0x7F, 0x81, 0x79, 0x86}};
static const EFI_GUID gEfiEdidDiscoveredGuid = {
    0x1C0C34F6, 0xD380, 0x41FA, {0xA0, 0x49, 0x8A, 0xD0, 0x6C, 0x1A, 0x66, 0xAA}};
typedef struct {
  UINT32 SizeOfEdid;
  UINT8 *Edid;
} EFI_EDID_PROTOCOL_MIN;
static UINT8 g_edid[256];
static UINT32 g_edid_len;
const EFI_GUID gEfiFileInfoGuid = {
    0x09576e92,
    0x6d3f,
    0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

/* ---- Helpers de memoria/strings ---- */
void zero_mem(VOID *dst, UINTN n) {
  UINT8 *p = (UINT8 *)dst;
  while (n--)
    *p++ = 0;
}
void copy_mem(VOID *dst, const VOID *src, UINTN n) {
  UINT8 *d = (UINT8 *)dst;
  const UINT8 *s = (const UINT8 *)src;
  while (n--)
    *d++ = *s++;
}
int compare_mem(const VOID *a, const VOID *b, UINTN n) {
  const UINT8 *pa = (const UINT8 *)a, *pb = (const UINT8 *)b;
  while (n--) {
    if (*pa != *pb)
      return (int)*pa - (int)*pb;
    pa++;
    pb++;
  }
  return 0;
}
int str_cmp_16(const CHAR16 *a, const CHAR16 *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(*a - *b);
}

/* ---- efi_print: subset de gnu-efi Print ----
 * Soporta: %s (CHAR16*), %d (int/INT64), %lx (UINT64 hex),
 *          %x (UINT32 hex), %u (UINT32 decimal), %c (char),
 *          %r (EFI_STATUS como 0x...), %% */
static CHAR16 *p_u64_dec(CHAR16 *out, UINT64 v) {
  CHAR16 tmp[24];
  int n = 0;
  if (v == 0)
    tmp[n++] = L'0';
  while (v) {
    tmp[n++] = (CHAR16)(L'0' + (v % 10));
    v /= 10;
  }
  while (n > 0)
    *out++ = tmp[--n];
  return out;
}
static CHAR16 *p_u64_hex(CHAR16 *out, UINT64 v) {
  CHAR16 tmp[24];
  int n = 0;
  if (v == 0)
    tmp[n++] = L'0';
  while (v) {
    int d = (int)(v & 0xF);
    tmp[n++] = (d < 10) ? (CHAR16)(L'0' + d) : (CHAR16)(L'a' + d - 10);
    v >>= 4;
  }
  while (n > 0)
    *out++ = tmp[--n];
  return out;
}

static CHAR16 *emit_ch(CHAR16 *out, CHAR16 *end, CHAR16 c) {
  if (c == L'\n') {
    if (out + 2 < end) {
      *out++ = L'\r';
      *out++ = L'\n';
    }
  } else {
    if (out + 1 < end) {
      *out++ = c;
    }
  }
  return out;
}

void efi_print(const CHAR16 *fmt, ...) {
  static CHAR16 buf[1024];
  CHAR16 *out = buf;
  CHAR16 *end = buf + 1022;

  va_list ap;
  va_start(ap, fmt);

  while (*fmt && out < end) {
    if (*fmt != L'%') {
      out = emit_ch(out, end, *fmt++);
      continue;
    }
    fmt++;
    if (out + 32 >= end)
      break;

    switch (*fmt) {
    case L's': {
      const CHAR16 *s = va_arg(ap, const CHAR16 *);
      if (!s)
        s = L"(null)";
      while (*s && out < end)
        out = emit_ch(out, end, *s++);
      break;
    }
    case L'd':
    case L'i': {
      INT64 v = va_arg(ap, INT64);
      if (v < 0) {
        out = emit_ch(out, end, L'-');
        v = -v;
      }
      CHAR16 tmp[24];
      int n = 0;
      if (v == 0)
        tmp[n++] = L'0';
      while (v) {
        tmp[n++] = (CHAR16)(L'0' + (v % 10));
        v /= 10;
      }
      while (n > 0)
        out = emit_ch(out, end, tmp[--n]);
      break;
    }
    case L'l': {
      CHAR16 c = *(fmt + 1);
      if (c == L'x' || c == L'X') {
        fmt++;
        UINT64 v = va_arg(ap, UINT64);
        out = p_u64_hex(out, v);
      } else if (c == L'u') {
        fmt++;
        UINT64 v = va_arg(ap, UINT64);
        out = p_u64_dec(out, v);
      } else {
        out = emit_ch(out, end, L'?');
      }
      break;
    }
    case L'u': {
      UINT32 v = va_arg(ap, UINT32);
      out = p_u64_dec(out, (UINT64)v);
      break;
    }
    case L'c': {
      int c = va_arg(ap, int);
      if (c == 0)
        c = L'.';
      out = emit_ch(out, end, (CHAR16)c);
      break;
    }
    case L'x':
    case L'X': {
      UINT32 v = va_arg(ap, UINT32);
      out = p_u64_hex(out, (UINT64)v);
      break;
    }
    case L'r': {
      UINT64 v = va_arg(ap, UINT64);
      out = emit_ch(out, end, L'0');
      out = emit_ch(out, end, L'x');
      out = p_u64_hex(out, v);
      break;
    }
    case L'%':
      out = emit_ch(out, end, L'%');
      break;
    default:
      out = emit_ch(out, end, L'?');
      break;
    }
    fmt++;
  }
  *out = 0;
  va_end(ap);

  if (gST && gST->ConOut) {
    gST->ConOut->OutputString(gST->ConOut, buf);
  }
}

/* ---- Constantes del bootloader ---- */
#define KERNEL_PATH_DEFAULT L"\\aurora\\kernel.elf"
#define INITRD_PATH_DEFAULT L"\\aurora\\initrd.tar"
#define CONFIG_MAX_SIZE 4096
#define KERNEL_PATH_MAX 256
#define INITRD_PATH_MAX 256
#define CMDLINE_MAX 256
#define ET_EXEC 2

/* Config parseada de aurora.conf. Los defaults se aplican si el
 * fichero no existe o está vacío. */
typedef struct {
  CHAR16 kernel_path[KERNEL_PATH_MAX];
  CHAR16 initrd_path[INITRD_PATH_MAX];
  CHAR8 cmdline[CMDLINE_MAX];
  BOOLEAN have_kernel;
  BOOLEAN have_initrd;
  BOOLEAN have_cmdline;
  UINT32 timeout;
} boot_config_t;

/* ---- ELF64 ---- */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;

#define EI_NIDENT 16
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define EM_X86_64 62
#define PT_LOAD 1

struct Elf64_Ehdr {
  unsigned char e_ident[EI_NIDENT];
  Elf64_Half e_type;
  Elf64_Half e_machine;
  Elf64_Word e_version;
  Elf64_Addr e_entry;
  Elf64_Off e_phoff;
  Elf64_Off e_shoff;
  Elf64_Word e_flags;
  Elf64_Half e_ehsize;
  Elf64_Half e_phentsize;
  Elf64_Half e_phnum;
  Elf64_Half e_shentsize;
  Elf64_Half e_shnum;
  Elf64_Half e_shstrndx;
};

struct Elf64_Phdr {
  Elf64_Word p_type;
  Elf64_Word p_flags;
  Elf64_Off p_offset;
  Elf64_Addr p_vaddr;
  Elf64_Addr p_paddr;
  Elf64_Xword p_filesz;
  Elf64_Xword p_memsz;
  Elf64_Xword p_align;
};

typedef struct {
  UINTN map_size;
  UINTN map_key;
  UINTN desc_size;
  UINT32 desc_version;
  EFI_MEMORY_DESCRIPTOR *map;
} mem_map_t;

static mem_map_t mem_map = {0};
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

/* ---- Memory map ---- */
static EFI_STATUS get_memory_map(EFI_HANDLE image_handle) {
  (void)image_handle;
  EFI_STATUS status;

  mem_map.map_size = 0;
  status = gBS->GetMemoryMap(&mem_map.map_size, NULL, &mem_map.map_key,
                             &mem_map.desc_size, &mem_map.desc_version);
  if (status != EFI_BUFFER_TOO_SMALL)
    return status;

  mem_map.map_size += 4 * mem_map.desc_size;
  status =
      gBS->AllocatePool(EfiLoaderData, mem_map.map_size, (VOID **)&mem_map.map);
  if (EFI_ERROR(status))
    return status;

  status = gBS->GetMemoryMap(&mem_map.map_size, mem_map.map, &mem_map.map_key,
                             &mem_map.desc_size, &mem_map.desc_version);
  return status;
}

/* ---- Mapeo 4K ---- */
static EFI_STATUS map_page_4k(UINT64 *pml4, EFI_PHYSICAL_ADDRESS virt,
                              EFI_PHYSICAL_ADDRESS phys) {
  UINTN pml4_idx = (virt >> 39) & 0x1FF;
  UINTN pdpt_idx = (virt >> 30) & 0x1FF;
  UINTN pd_idx = (virt >> 21) & 0x1FF;
  UINTN pt_idx = (virt >> 12) & 0x1FF;

  if (!(pml4[pml4_idx] & 1)) {
    EFI_PHYSICAL_ADDRESS new_pdpt = 0;
    gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &new_pdpt);
    zero_mem((VOID *)new_pdpt, 4096);
    pml4[pml4_idx] = new_pdpt | 0x03;
  }
  UINT64 *pdpt = (UINT64 *)(pml4[pml4_idx] & ~0xFFFULL);

  if (!(pdpt[pdpt_idx] & 1)) {
    EFI_PHYSICAL_ADDRESS new_pd = 0;
    gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &new_pd);
    zero_mem((VOID *)new_pd, 4096);
    pdpt[pdpt_idx] = new_pd | 0x03;
  }
  UINT64 *pd = (UINT64 *)(pdpt[pdpt_idx] & ~0xFFFULL);

  if (!(pd[pd_idx] & 1)) {
    EFI_PHYSICAL_ADDRESS new_pt = 0;
    gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &new_pt);
    zero_mem((VOID *)new_pt, 4096);
    pd[pd_idx] = new_pt | 0x03;
  }
  UINT64 *pt = (UINT64 *)(pd[pd_idx] & ~0xFFFULL);

  pt[pt_idx] = phys | 0x03;
  return EFI_SUCCESS;
}

/* ===========================================================================
 * Búsqueda de ficheros en volúmenes SimpleFileSystem
 * ===========================================================================
 */
static EFI_STATUS find_file_in_any_volume(EFI_HANDLE image_handle,
                                          const CHAR16 *path,
                                          EFI_FILE **out_file,
                                          EFI_FILE **out_root) {
  EFI_HANDLE *handles = NULL;
  UINTN handle_count = 0;

  EFI_STATUS status = gBS->LocateHandleBuffer(
      ByProtocol, (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, NULL,
      &handle_count, &handles);
  if (EFI_ERROR(status) || !handles || handle_count == 0) {
    return EFI_NOT_FOUND;
  }

  for (UINTN i = 0; i < handle_count; i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs_proto = NULL;
    status = gBS->OpenProtocol(
        handles[i], (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&fs_proto, image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(status) || !fs_proto)
      continue;

    EFI_FILE *root = NULL;
    status = fs_proto->OpenVolume(fs_proto, &root);
    if (EFI_ERROR(status) || !root)
      continue;

    EFI_FILE *file = NULL;
    status = root->Open(root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(status) && file) {
      if (out_file)
        *out_file = file;
      if (out_root)
        *out_root = root;
      else
        root->Close(root);
      gBS->FreePool(handles);
      return EFI_SUCCESS;
    }

    root->Close(root);
  }

  gBS->FreePool(handles);
  return EFI_NOT_FOUND;
}

/* ===========================================================================
 * Parser del .conf
 * ===========================================================================
 */

/* Copia un CHAR16* a un buffer fijo con truncado seguro. */
static VOID cfg_copy_path(CHAR16 *dst, UINTN dst_max, const CHAR16 *src) {
  UINTN i = 0;
  while (src[i] && i + 1 < dst_max) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

static VOID boot_config_defaults(boot_config_t *cfg) {
  zero_mem(cfg, sizeof(*cfg));
  cfg_copy_path(cfg->kernel_path, KERNEL_PATH_MAX, KERNEL_PATH_DEFAULT);
  cfg_copy_path(cfg->initrd_path, INITRD_PATH_MAX, INITRD_PATH_DEFAULT);
  cfg->timeout = 3;
}

/* Devuelve TRUE si la línea (ya sin comentarios ni espacios líderes)
 * empieza por "<want>=" (o "<want> ="). NO consume el '='. */
static BOOLEAN cfg_key_match(const CHAR8 *line, UINTN line_len,
                             const CHAR8 *want) {
  UINTN want_len = 0;
  while (want[want_len])
    want_len++;

  UINTN eq = 0;
  while (eq < line_len && line[eq] != '=')
    eq++;
  if (eq >= line_len)
    return FALSE;

  UINTN ke = eq;
  while (ke > 0 && (line[ke - 1] == ' ' || line[ke - 1] == '\t'))
    ke--;
  if (ke != want_len)
    return FALSE;
  for (UINTN i = 0; i < want_len; i++) {
    if (line[i] != want[i])
      return FALSE;
  }
  return TRUE;
}

/* Extrae el valor tras '=', recorta whitespace, normaliza '/'->'\' y
 * prefija con '\' si falta. Copia a out como CHAR16. */
static VOID cfg_extract_path_value(const CHAR8 *line, UINTN line_len,
                                   CHAR16 *out, UINTN out_max) {
  UINTN eq = 0;
  while (eq < line_len && line[eq] != '=')
    eq++;
  if (eq >= line_len) {
    out[0] = 0;
    return;
  }

  UINTN vs = eq + 1;
  while (vs < line_len && (line[vs] == ' ' || line[vs] == '\t'))
    vs++;
  UINTN ve = line_len;
  while (ve > vs &&
         (line[ve - 1] == ' ' || line[ve - 1] == '\t' || line[ve - 1] == '\r'))
    ve--;
  if (vs >= ve) {
    out[0] = 0;
    return;
  }

  UINTN oi = 0;
  CHAR8 first = line[vs];
  if (first != '\\' && first != '/') {
    if (oi + 1 < out_max)
      out[oi++] = L'\\';
  }
  while (vs < ve && oi + 1 < out_max) {
    CHAR8 c = line[vs++];
    if (c == '/')
      c = '\\';
    out[oi++] = (CHAR16)c;
  }
  out[oi] = 0;
}

/* Igual que cfg_extract_path_value pero deja el valor como CHAR8 (para
 * cmdline, que es texto libre). */
static VOID cfg_extract_raw_value(const CHAR8 *line, UINTN line_len, CHAR8 *out,
                                  UINTN out_max) {
  UINTN eq = 0;
  while (eq < line_len && line[eq] != '=')
    eq++;
  if (eq >= line_len) {
    out[0] = 0;
    return;
  }
  UINTN vs = eq + 1;
  while (vs < line_len && (line[vs] == ' ' || line[vs] == '\t'))
    vs++;
  UINTN ve = line_len;
  while (ve > vs &&
         (line[ve - 1] == ' ' || line[ve - 1] == '\t' || line[ve - 1] == '\r'))
    ve--;
  UINTN oi = 0;
  while (vs < ve && oi + 1 < out_max)
    out[oi++] = line[vs++];
  out[oi] = 0;
}

/* Lee aurora.conf y rellena cfg. Si no se encuentra o falla, deja los
 * defaults. NUNCA falla. */
static VOID load_boot_config(EFI_FILE *root, boot_config_t *cfg) {
  boot_config_defaults(cfg);
  if (!root)
    return;

  static const CHAR16 *conf_candidates[] = {
      L"\\aurora\\aurora.conf",
      L"\\AURORA\\AURORA.CONF",
      L"\\etc\\aurora.conf",
      L"\\ETC\\AURORA.CONF",
  };

  EFI_FILE *file = NULL;
  for (int c = 0; c < 4 && !file; c++) {
    EFI_STATUS st = root->Open(root, &file, (CHAR16 *)conf_candidates[c],
                               EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !file)
      file = NULL;
  }
  if (!file) {
    efi_print(L"[BOOT] Config no encontrado, usando defaults\n");
    return;
  }

  static CHAR8 buf[CONFIG_MAX_SIZE];
  UINTN size = sizeof(buf) - 1;
  EFI_STATUS status = file->Read(file, &size, buf);
  file->Close(file);
  if (EFI_ERROR(status) || size == 0) {
    efi_print(L"[BOOT] WARN: config vacío o ilegible\n");
    return;
  }
  buf[size] = 0;

  UINTN i = 0;
  while (i < size) {
    UINTN line_start = i;
    while (i < size && buf[i] != '\n')
      i++;
    UINTN line_end = i;
    if (i < size)
      i++;

    while (line_end > line_start && buf[line_end - 1] == '\r')
      line_end--;
    UINTN ls = line_start;
    while (ls < line_end && (buf[ls] == ' ' || buf[ls] == '\t'))
      ls++;
    if (ls >= line_end || buf[ls] == '#')
      continue;

    UINTN len = line_end - ls;
    const CHAR8 *line = buf + ls;

    if (cfg_key_match(line, len, "kernel")) {
      cfg_extract_path_value(line, len, cfg->kernel_path, KERNEL_PATH_MAX);
      cfg->have_kernel = TRUE;
      efi_print(L"[BOOT] Config: kernel=%s\n", cfg->kernel_path);
    } else if (cfg_key_match(line, len, "initrd")) {
      cfg_extract_path_value(line, len, cfg->initrd_path, INITRD_PATH_MAX);
      cfg->have_initrd = TRUE;
      efi_print(L"[BOOT] Config: initrd=%s\n", cfg->initrd_path);
    } else if (cfg_key_match(line, len, "cmdline")) {
      cfg_extract_raw_value(line, len, cfg->cmdline, CMDLINE_MAX);
      cfg->have_cmdline = TRUE;
    } else if (cfg_key_match(line, len, "timeout")) {
      UINTN eq = 0;
      while (eq < len && line[eq] != '=')
        eq++;
      UINTN vs = eq + 1;
      UINT32 t = 0;
      while (vs < len && line[vs] >= '0' && line[vs] <= '9') {
        t = t * 10 + (UINT32)(line[vs] - '0');
        vs++;
      }
      cfg->timeout = t;
    }
  }
}

/* ===========================================================================
 * Carga del kernel (ELF64 ET_EXEC)
 * ===========================================================================
 */
static EFI_STATUS load_kernel(EFI_FILE *file, VOID **entry_point,
                              UINT64 *pml4) {
  if (!file)
    return EFI_INVALID_PARAMETER;

  struct Elf64_Ehdr ehdr;
  UINTN size = sizeof(ehdr);
  EFI_STATUS status = file->Read(file, &size, &ehdr);
  if (EFI_ERROR(status) || size != sizeof(ehdr)) {
    efi_print(L"[BOOT] No se pudo leer cabecera ELF\n");
    return EFI_LOAD_ERROR;
  }

  if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
      ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
    efi_print(L"[BOOT] No es un ELF válido\n");
    return EFI_INVALID_PARAMETER;
  }
  if (ehdr.e_machine != EM_X86_64) {
    efi_print(L"[BOOT] ELF no es x86_64 (machine=%d)\n", ehdr.e_machine);
    return EFI_UNSUPPORTED;
  }
  if (ehdr.e_type != ET_EXEC) {
    efi_print(L"[BOOT] ELF no es ET_EXEC (type=%d)\n", ehdr.e_type);
    return EFI_UNSUPPORTED;
  }

  for (UINTN i = 0; i < ehdr.e_phnum; i++) {
    struct Elf64_Phdr phdr;
    UINTN phdr_size = sizeof(phdr);
    status = file->SetPosition(file, ehdr.e_phoff + i * ehdr.e_phentsize);
    if (EFI_ERROR(status))
      return status;
    status = file->Read(file, &phdr_size, &phdr);
    if (EFI_ERROR(status))
      return status;

    if (phdr.p_type != PT_LOAD)
      continue;

    EFI_PHYSICAL_ADDRESS paddr = phdr.p_paddr;
    if (paddr == 0) {
      status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                  (phdr.p_memsz + 0xFFF) / 0x1000, &paddr);
    } else {
      UINTN num_pages = (phdr.p_memsz + 0xFFF) / 0x1000;
      if (num_pages == 0)
        num_pages = 1;
      status =
          gBS->AllocatePages(AllocateAddress, EfiLoaderData, num_pages, &paddr);
      if (EFI_ERROR(status)) {
        status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, num_pages,
                                    &paddr);
      }
    }
    if (EFI_ERROR(status)) {
      efi_print(L"[BOOT] Fallo alocando segmento %d\n", i);
      return status;
    }

    status = file->SetPosition(file, phdr.p_offset);
    if (EFI_ERROR(status))
      return status;
    UINTN read_size = phdr.p_filesz;
    status = file->Read(file, &read_size, (VOID *)paddr);
    if (EFI_ERROR(status))
      return status;

    if (phdr.p_memsz > phdr.p_filesz) {
      zero_mem((VOID *)(paddr + phdr.p_filesz), phdr.p_memsz - phdr.p_filesz);
    }

    UINTN num_pages = (phdr.p_memsz + 0xFFF) / 0x1000;
    for (UINTN p = 0; p < num_pages; p++) {
      map_page_4k(pml4, phdr.p_vaddr + p * 0x1000, paddr + p * 0x1000);
    }

    efi_print(L"[BOOT] Segmento %d: vaddr=0x%lx paddr=0x%lx pages=%d\n", i,
              phdr.p_vaddr, paddr, num_pages);
  }

  *entry_point = (VOID *)ehdr.e_entry;
  efi_print(L"[BOOT] Entry point: 0x%lx\n", ehdr.e_entry);
  return EFI_SUCCESS;
}

/* Carga un initrd ya abierto. Cierra el fichero siempre (éxito o
 * fallo). Aloja la memoria con AllocateMaxAddress(0xFFFFFFFF) para
 * garantizar < 4 GiB, que es lo que el kernel necesita para
 * phys_to_virt(). */
static EFI_STATUS load_initrd_from_file(EFI_FILE *file,
                                        EFI_PHYSICAL_ADDRESS *out_addr,
                                        UINTN *out_size) {
  if (!file || !out_addr || !out_size)
    return EFI_INVALID_PARAMETER;

  UINT8 info_buf[512];
  UINTN info_size = sizeof(info_buf);
  EFI_STATUS st =
      file->GetInfo(file, (EFI_GUID *)&gEfiFileInfoGuid, &info_size, info_buf);
  if (EFI_ERROR(st) || info_size < sizeof(EFI_FILE_INFO)) {
    file->Close(file);
    return EFI_LOAD_ERROR;
  }

  EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
  UINTN fsize = (UINTN)info->FileSize;
  if (fsize == 0) {
    file->Close(file);
    return EFI_LOAD_ERROR;
  }

  UINTN npages = (fsize + 0xFFF) / 0x1000;
  EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFF;
  st = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, npages, &addr);
  if (EFI_ERROR(st)) {
    file->Close(file);
    return st;
  }

  /* --- LECTURA TROCEADA PARA QUE LA ANIMACION NO SE CONGELE --- */
  // Reducido de 4MB a 256KB para asegurar 60FPS en la animación
  UINTN chunk_size = 1024 * 1024; // 256 KB por chunk
  UINTN total_read = 0;

  while (total_read < fsize) {
    UINTN to_read = fsize - total_read;
    if (to_read > chunk_size)
      to_read = chunk_size;
    UINTN read_size = to_read;

    st = file->Read(file, &read_size, (VOID *)(addr + total_read));
    if (EFI_ERROR(st) || read_size == 0) {
      file->Close(file);
      gBS->FreePages(addr, npages);
      return EFI_LOAD_ERROR;
    }
    total_read += read_size;

    // Animar mientras carga
    efi_splash_tick();
  }

  file->Close(file);
  efi_print(L"[BOOT] initrd: %lu bytes @ phys=0x%lx\n", (UINT64)fsize,
            (UINT64)addr);
  *out_addr = addr;
  *out_size = fsize;
  return EFI_SUCCESS;
}

/* ===========================================================================
 * Debug: listado de volúmenes y paths conocidos
 * ===========================================================================
 */
static EFI_STATUS list_root_files(EFI_FILE *root, const CHAR16 *label) {
  efi_print(L"[DUMP] === %s ===\n", label);

  static const CHAR16 *probes[] = {
      L"\\EFI",
      L"\\EFI\\BOOT",
      L"\\EFI\\BOOT\\BOOTX64.EFI",
      L"\\aurora",
      L"\\aurora\\kernel.elf",
      L"\\aurora\\initrd.tar",
      L"\\aurora\\aurora.conf",
      L"\\AURORA",
      L"\\AURORA\\KERNEL.ELF",
      L"\\AURORA\\INITRD.TAR",
      L"\\kernel.elf",
      L"\\KERNEL.ELF",
  };

  for (int i = 0; i < 12; i++) {
    EFI_FILE *f = NULL;
    EFI_STATUS st =
        root->Open(root, &f, (CHAR16 *)probes[i], EFI_FILE_MODE_READ, 0);
    efi_print(L"[DUMP]   %s : %r\n", probes[i], st);
    if (!EFI_ERROR(st) && f)
      f->Close(f);
  }
  return EFI_SUCCESS;
}

static void dump_all_volumes(EFI_HANDLE image_handle) {
  EFI_HANDLE *handles = NULL;
  UINTN count = 0;
  EFI_STATUS status = gBS->LocateHandleBuffer(
      ByProtocol, (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, NULL, &count,
      &handles);
  if (EFI_ERROR(status) || !handles) {
    efi_print(L"[DUMP] No hay volúmenes SimpleFileSystem\n");
    return;
  }
  efi_print(L"[DUMP] Volúmenes encontrados: %lu\n", (UINT64)count);
  for (UINTN i = 0; i < count; i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs_proto = NULL;
    status = gBS->OpenProtocol(
        handles[i], (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&fs_proto, image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(status) || !fs_proto)
      continue;
    EFI_FILE *root = NULL;
    if (EFI_ERROR(fs_proto->OpenVolume(fs_proto, &root)) || !root)
      continue;

    CHAR16 label[8];
    label[0] = L'V';
    label[1] = (CHAR16)(L'0' + (i / 10));
    label[2] = (CHAR16)(L'0' + (i % 10));
    label[3] = 0;
    list_root_files(root, label);
    root->Close(root);
  }
  gBS->FreePool(handles);
}

/* ===========================================================================
 * Framebuffer
 * ===========================================================================
 */
/* Lee el EDID del monitor (activo; si no, el descubierto). Solo copia un EDID
 * con cabecera valida y checksum correcto en cada bloque de 128 bytes. */
static void read_edid(void) {
  const EFI_GUID *guids[2] = {&gEfiEdidActiveGuid, &gEfiEdidDiscoveredGuid};
  g_edid_len = 0;
  for (int g = 0; g < 2 && !g_edid_len; g++) {
    EFI_EDID_PROTOCOL_MIN *p = NULL;
    if (EFI_ERROR(gBS->LocateProtocol((EFI_GUID *)guids[g], NULL, (VOID **)&p)) ||
        !p || !p->Edid || p->SizeOfEdid < 128)
      continue;
    UINT32 n = p->SizeOfEdid > 256 ? 256 : p->SizeOfEdid;
    n &= ~127u;
    static const UINT8 hdr[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (compare_mem(p->Edid, hdr, 8) != 0)
      continue;
    int ok = 1;
    for (UINT32 b = 0; b < n; b += 128) {
      UINT8 sum = 0;
      for (UINT32 i = 0; i < 128; i++)
        sum += p->Edid[b + i];
      if (sum != 0)
        ok = 0;
    }
    if (!ok)
      continue;
    copy_mem(g_edid, p->Edid, n);
    g_edid_len = n;
  }
  if (g_edid_len)
    efi_print(L"[BOOT] EDID: %u bytes\n", g_edid_len);
  else
    efi_print(L"[BOOT] EDID no disponible\n");
}

/* Resolucion nativa (primer descriptor de 18 bytes = modo preferido). */
static int edid_preferred(UINT32 *w, UINT32 *h) {
  if (!g_edid_len)
    return 0;
  const UINT8 *d = &g_edid[54];
  if (d[0] == 0 && d[1] == 0)
    return 0; /* no es un descriptor de temporizacion */
  *w = d[2] | ((UINT32)(d[4] & 0xF0) << 4);
  *h = d[5] | ((UINT32)(d[7] & 0xF0) << 4);
  return *w && *h;
}

static EFI_STATUS setup_framebuffer(void) {
  read_edid();
  EFI_STATUS status = gBS->LocateProtocol(
      (EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
  if (EFI_ERROR(status) || !gop) {
    efi_print(L"[BOOT] GOP no disponible\n");
    return status;
  }

  // Definir tipos de función para QueryMode y SetMode
  typedef EFI_STATUS (*EFI_GOP_QUERY_MODE)(
      VOID *, UINT32, UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
  typedef EFI_STATUS (*EFI_GOP_SET_MODE)(VOID *, UINT32);

  EFI_GOP_QUERY_MODE query_mode = (EFI_GOP_QUERY_MODE)gop->QueryMode;
  EFI_GOP_SET_MODE set_mode = (EFI_GOP_SET_MODE)gop->SetMode;

  if (query_mode && set_mode && gop->Mode) {
    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_area = 0;
    UINTN best_width = gop->Mode->Info->HorizontalResolution;
    UINTN best_height = gop->Mode->Info->VerticalResolution;

    // Limitar el modo actual si ya es menor a 1080p
    if (best_width <= 1920 && best_height <= 1080) {
      best_area = best_width * best_height;
    } else {
      best_area = 0; // Forzar búsqueda si el actual es muy grande
    }

    // Modo nativo del monitor (EDID) si la GOP lo ofrece y no pasa de
    // 1920x1200 (el compositor trabaja por software).
    UINT32 nat_w = 0, nat_h = 0, nat_mode = 0xFFFFFFFFu;
    if (edid_preferred(&nat_w, &nat_h) && nat_w <= 1920 && nat_h <= 1200) {
      for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        UINTN size;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (query_mode(gop, i, &size, &info) == EFI_SUCCESS && info &&
            info->HorizontalResolution == nat_w &&
            info->VerticalResolution == nat_h && info->PixelFormat != 3) {
          nat_mode = i;
          break;
        }
      }
      if (nat_mode != 0xFFFFFFFFu)
        efi_print(L"[BOOT] Modo nativo del monitor: %ux%u (modo %u)\n", nat_w,
                  nat_h, nat_mode);
    }

    // Iterar por todos los modos disponibles
    for (UINT32 i = 0; nat_mode == 0xFFFFFFFFu && i < gop->Mode->MaxMode; i++) {
      UINTN size;
      EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
      if (query_mode(gop, i, &size, &info) == EFI_SUCCESS && info) {
        // Comprobar si es <= 1080p y formato de 32 bits
        if (info->HorizontalResolution <= 1920 &&
            info->VerticalResolution <= 1080 &&
            info->PixelFormat != 3 /* PixelBltOnly */) {

          UINT32 area = info->HorizontalResolution * info->VerticalResolution;
          if (area > best_area) {
            best_area = area;
            best_mode = i;
          }
        }
      }
    }

    if (nat_mode != 0xFFFFFFFFu) {
      best_mode = nat_mode;
      best_area = nat_w * nat_h;
    }
    // Si encontramos un modo mejor que el actual, lo aplicamos
    if (best_area > 0 && best_mode != gop->Mode->Mode) {
      efi_print(L"[BOOT] Cambiando a modo %u (%ux%u)\n", best_mode,
                gop->Mode->Info->HorizontalResolution,
                gop->Mode->Info->VerticalResolution);
      set_mode(gop, best_mode);
    }
  }

  efi_print(L"[BOOT] FB: %dx%d @ 0x%lx size=0x%lx pitch=%d\n",
            gop->Mode->Info->HorizontalResolution,
            gop->Mode->Info->VerticalResolution, gop->Mode->FrameBufferBase,
            gop->Mode->FrameBufferSize, gop->Mode->Info->PixelsPerScanLine * 4);
  return EFI_SUCCESS;
}

/* ===========================================================================
 * Page tables
 * ===========================================================================
 */
static EFI_STATUS build_page_tables(EFI_PHYSICAL_ADDRESS *pml4_out) {
  EFI_STATUS status;
  EFI_PHYSICAL_ADDRESS pml4_addr = 0xFFFFFFFF;
  EFI_PHYSICAL_ADDRESS pdpt_addr = 0xFFFFFFFF;
  EFI_PHYSICAL_ADDRESS pd_addrs[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                      0xFFFFFFFF};

  status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &pml4_addr);
  if (EFI_ERROR(status))
    return status;
  status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &pdpt_addr);
  if (EFI_ERROR(status))
    return status;
  for (int i = 0; i < 4; i++) {
    status =
        gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &pd_addrs[i]);
    if (EFI_ERROR(status))
      return status;
  }

  UINT64 *pml4 = (UINT64 *)pml4_addr;
  UINT64 *pdpt = (UINT64 *)pdpt_addr;

  zero_mem(pml4, 4096);
  zero_mem(pdpt, 4096);
  for (int i = 0; i < 4; i++)
    zero_mem((VOID *)pd_addrs[i], 4096);

  pml4[0] = pdpt_addr | 0x03;
  for (int pd_idx = 0; pd_idx < 4; pd_idx++) {
    pdpt[pd_idx] = pd_addrs[pd_idx] | 0x03;
    UINT64 *pd = (UINT64 *)pd_addrs[pd_idx];
    for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
      uint64_t phys = (uint64_t)pd_idx * 0x40000000 + pt_idx * 0x200000;
      pd[pt_idx] = phys | 0x83;
    }
  }

  *pml4_out = pml4_addr;
  return EFI_SUCCESS;
}

/* ===========================================================================
 * ACPI RSDP
 * ===========================================================================
 */
static BOOLEAN guid_equals(const EFI_GUID *a, const EFI_GUID *b) {
  return (BOOLEAN)(compare_mem(a, b, sizeof(EFI_GUID)) == 0);
}

static void find_acpi_rsdp(EFI_SYSTEM_TABLE *st, uint8_t *out_rsdp) {
  EFI_GUID acpi20_guid = {0x8868e871,
                          0xe4f1,
                          0x11d3,
                          {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
  EFI_GUID acpi10_guid = {0xeb9d2d30,
                          0x2d88,
                          0x11d3,
                          {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

  for (int j = 0; j < 64; j++)
    out_rsdp[j] = 0;

  const EFI_GUID *guids_to_try[2] = {&acpi20_guid, &acpi10_guid};

  for (int g = 0; g < 2; g++) {
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
      EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
      if (!guid_equals(&ct->VendorGuid, guids_to_try[g]))
        continue;

      uint8_t *rsdp = (uint8_t *)ct->VendorTable;
      if (!rsdp)
        continue;

      if (rsdp[0] != 'R' || rsdp[1] != 'S' || rsdp[2] != 'D' ||
          rsdp[3] != ' ' || rsdp[4] != 'P' || rsdp[5] != 'T' ||
          rsdp[6] != 'R' || rsdp[7] != ' ') {
        efi_print(L"[BOOT] ACPI RSDP firma inválida\n");
        continue;
      }

      for (int j = 0; j < 64; j++)
        out_rsdp[j] = rsdp[j];

      efi_print(L"[BOOT] ACPI RSDP encontrado (%s), revision=%d\n",
                g == 0 ? L"ACPI 2.0+" : L"ACPI 1.0", rsdp[15]);
      return;
    }
  }
  efi_print(L"[BOOT] ACPI RSDP no encontrado\n");
}

/* ===========================================================================
 * Salto al kernel
 * ===========================================================================
 */
static void jump_to_kernel(VOID *kernel_entry, EFI_HANDLE image_handle,
                           struct kernel_boot_info *kinfo,
                           EFI_PHYSICAL_ADDRESS pml4_addr) {
  EFI_STATUS status;

  EFI_PHYSICAL_ADDRESS tramp_addr = 0xFFFFFFFF;
  status =
      gBS->AllocatePages(AllocateMaxAddress, EfiLoaderCode, 1, &tramp_addr);
  if (EFI_ERROR(status)) {
    efi_print(L"[BOOT] trampoline alloc FAIL %r\n", status);
    return;
  }
  static const UINT8 tramp_code[] = {
      0x0F, 0x22, 0xD9, /* mov cr3, rcx */
      0x48, 0x89, 0xD7, /* mov rdi, rdx */
      0x41, 0xFF, 0xE0  /* jmp r8 */
  };
  copy_mem((VOID *)tramp_addr, tramp_code, sizeof(tramp_code));

  EFI_PHYSICAL_ADDRESS kinfo_addr = 0xFFFFFFFF;
  status =
      gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &kinfo_addr);
  if (EFI_ERROR(status)) {
    efi_print(L"[BOOT] kinfo alloc FAIL %r\n", status);
    return;
  }
  copy_mem((VOID *)kinfo_addr, kinfo, sizeof(*kinfo));
  kinfo = (struct kernel_boot_info *)kinfo_addr;

  EFI_PHYSICAL_ADDRESS safe_memmap_addr = 0;
  UINTN safe_pages = (mem_map.map_size + 0xFFF + 65536) / 0x1000;
  if (safe_pages < 16)
    safe_pages = 16;
  status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, safe_pages,
                              &safe_memmap_addr);
  if (EFI_ERROR(status)) {
    efi_print(L"[BOOT] safe_memmap alloc FAIL %r\n", status);
    return;
  }
  efi_print(L"[BOOT] safe_memmap @ %lx (%lu pages)\n", (UINT64)safe_memmap_addr,
            (UINT64)safe_pages);

  __asm__ volatile("cli");

  EFI_MEMORY_DESCRIPTOR *map_buf = NULL;
  UINTN map_buf_size = 0;
  UINTN map_key = 0;
  UINTN desc_size = 0;
  UINT32 desc_ver = 0;
  status = EFI_INVALID_PARAMETER;

  for (int attempt = 0; attempt < 32; attempt++) {
    if (attempt > 0)
      gBS->Stall(1000);

    if (map_buf) {
      gBS->FreePool(map_buf);
      map_buf = NULL;
      map_buf_size = 0;
    }

    UINTN ms = 0;
    EFI_STATUS s =
        gBS->GetMemoryMap(&ms, NULL, &map_key, &desc_size, &desc_ver);
    if (s != EFI_BUFFER_TOO_SMALL) {
      efi_print(L"[BOOT] t%d GM(size) FAIL %r\n", (INT64)attempt, s);
      continue;
    }
    ms += 8 * desc_size;

    s = gBS->AllocatePool(EfiLoaderData, ms, (VOID **)&map_buf);
    if (EFI_ERROR(s) || !map_buf) {
      efi_print(L"[BOOT] t%d AllocPool FAIL %r\n", (INT64)attempt, s);
      map_buf = NULL;
      continue;
    }
    map_buf_size = ms;

    s = gBS->GetMemoryMap(&map_buf_size, map_buf, &map_key, &desc_size,
                          &desc_ver);
    if (EFI_ERROR(s)) {
      efi_print(L"[BOOT] t%d GM(buf) FAIL %r\n", (INT64)attempt, s);
      continue;
    }

    copy_mem((VOID *)safe_memmap_addr, map_buf, map_buf_size);
    kinfo->memmap = safe_memmap_addr;
    kinfo->memmap_size = map_buf_size;
    kinfo->memmap_desc_size = desc_size;
    kinfo->memmap_desc_ver = desc_ver;

    status = gBS->ExitBootServices(image_handle, map_key);
    if (!EFI_ERROR(status))
      break;

    efi_print(L"[BOOT] t%d ExitBootServices FAIL %r\n", (INT64)attempt, status);
  }

  if (EFI_ERROR(status)) {
    efi_print(L"[BOOT] ExitBootServices fallo tras 32 intentos. Halt.\n");
    while (1)
      __asm__ volatile("hlt");
  }

  typedef void (*__attribute__((ms_abi)) tramp_fn_t)(
      UINT64 pml4, struct kernel_boot_info *kinfo, VOID *entry);
  ((tramp_fn_t)tramp_addr)((UINT64)pml4_addr, kinfo, kernel_entry);

  while (1)
    __asm__ volatile("hlt");
}

/* ===========================================================================
 * Entry point
 * ===========================================================================
 */
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
  gImageHandle = image_handle;
  gST = system_table;
  gBS = system_table->BootServices;

  efi_print(L"\n=== AURORA OS BOOTLOADER ===\n");
  efi_print(L"[BOOT] Cargando kernel...\n");

  /* ---------------------------------------------------------------------
   * 1. Abrir el root de CUALQUIER volumen SimpleFileSystem para poder
   *    leer aurora.conf antes de saber dónde está el kernel.
   * --------------------------------------------------------------------- */
  EFI_FILE *cfg_root = NULL;
  {
    EFI_HANDLE *handles = NULL;
    UINTN n = 0;
    EFI_STATUS s = gBS->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, NULL, &n,
        &handles);
    if (!EFI_ERROR(s) && handles && n > 0) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fsp = NULL;
      s = gBS->OpenProtocol(
          handles[0], (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid,
          (VOID **)&fsp, image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
      if (!EFI_ERROR(s) && fsp)
        fsp->OpenVolume(fsp, &cfg_root);
      gBS->FreePool(handles);
    }
  }

  /* ---------------------------------------------------------------------
   * 2. Cargar config (kernel, initrd, cmdline, timeout).
   * --------------------------------------------------------------------- */
  static boot_config_t cfg;
  load_boot_config(cfg_root, &cfg);

  /* ---------------------------------------------------------------------
   * 3. Localizar el kernel: primero el path del .conf, después la lista
   *    de candidatos hardcodeados como fallback.
   * --------------------------------------------------------------------- */
  EFI_FILE *kernel_file = NULL;
  EFI_FILE *kernel_root = NULL;
  const CHAR16 *kernel_path_used = NULL;
  EFI_STATUS status = EFI_NOT_FOUND;

  if (cfg.have_kernel) {
    status = find_file_in_any_volume(image_handle, cfg.kernel_path,
                                     &kernel_file, &kernel_root);
    if (!EFI_ERROR(status))
      kernel_path_used = cfg.kernel_path;
  }

  if (EFI_ERROR(status) || !kernel_file) {
    static const CHAR16 *kernel_candidates[] = {
        L"\\aurora\\kernel.elf", L"\\AURORA\\KERNEL.ELF",    L"\\kernel.elf",
        L"\\KERNEL.ELF",         L"\\EFI\\BOOT\\kernel.elf",
    };
    for (int i = 0; i < 5; i++) {
      status = find_file_in_any_volume(image_handle, kernel_candidates[i],
                                       &kernel_file, &kernel_root);
      if (!EFI_ERROR(status)) {
        kernel_path_used = kernel_candidates[i];
        break;
      }
    }
  }

  if (EFI_ERROR(status) || !kernel_file) {
    efi_print(L"[BOOT] Kernel no encontrado. Dump de volúmenes:\n");
    dump_all_volumes(image_handle);
    efi_print(L"[BOOT] Halt.\n");
    return EFI_NOT_FOUND;
  }
  efi_print(L"[BOOT] Kernel: %s\n", kernel_path_used);

  /* ---------------------------------------------------------------------
   * 4. Framebuffer + page tables.
   * --------------------------------------------------------------------- */
  setup_framebuffer();

  /* Splash simple mientras se carga el initrd (que es lo lento). */
  int splash_on = 0;
  if (gop && gop->Mode && gop->Mode->Info) {
    EFI_PHYSICAL_ADDRESS lfb = gop->Mode->FrameBufferBase;
    UINT32 pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    splash_on =
        (efi_splash_init(lfb, gop->Mode->Info->HorizontalResolution,
                         gop->Mode->Info->VerticalResolution, pitch, 32) == 0);
    if (splash_on)
      efi_print(L"[SPLASH] activa %ux%u\n",
                gop->Mode->Info->HorizontalResolution,
                gop->Mode->Info->VerticalResolution);
  }

  EFI_PHYSICAL_ADDRESS pml4_addr = 0;
  status = build_page_tables(&pml4_addr);
  if (EFI_ERROR(status)) {
    efi_print(L"[BOOT] Fallo construyendo page tables\n");
    return status;
  }

  /* ---------------------------------------------------------------------
   * 5. Cargar los PT_LOAD del kernel.
   * --------------------------------------------------------------------- */
  VOID *kernel_entry = NULL;
  status = load_kernel(kernel_file, &kernel_entry, (UINT64 *)pml4_addr);
  if (EFI_ERROR(status)) {
    efi_print(L"[BOOT] Fallo al cargar kernel: %r\n", status);
    return status;
  }

  /* ---------------------------------------------------------------------
   * 6. Cargar el initrd desde el MISMO volumen que el kernel:
   *    - primero el path del .conf (si lo hay)
   *    - si no, los candidatos hardcodeados
   * --------------------------------------------------------------------- */
  EFI_PHYSICAL_ADDRESS initrd_phys = 0;
  UINTN initrd_size = 0;

  if (kernel_root) {
    EFI_FILE *initrd_file = NULL;

    if (cfg.have_initrd) {
      status = kernel_root->Open(kernel_root, &initrd_file, cfg.initrd_path,
                                 EFI_FILE_MODE_READ, 0);
      if (EFI_ERROR(status) || !initrd_file)
        initrd_file = NULL;
    }

    if (!initrd_file) {
      static const CHAR16 *initrd_candidates[] = {
          L"\\aurora\\initrd.tar",
          L"\\AURORA\\INITRD.TAR",
          L"\\initrd.tar",
          L"\\INITRD.TAR",
      };
      for (int i = 0; i < 4 && !initrd_file; i++) {
        EFI_STATUS s = kernel_root->Open(kernel_root, &initrd_file,
                                         (CHAR16 *)initrd_candidates[i],
                                         EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(s) || !initrd_file)
          initrd_file = NULL;
      }
    }

    if (initrd_file) {
      status = load_initrd_from_file(initrd_file, &initrd_phys, &initrd_size);
      if (EFI_ERROR(status)) {
        efi_print(L"[BOOT] WARN: initrd no cargado (%r)\n", status);
        initrd_phys = 0;
        initrd_size = 0;
      }
    } else {
      efi_print(L"[BOOT] WARN: initrd.tar no encontrado en el volumen\n");
    }
  }

  /* Si la splash está activa, darla un mínimo de vida visual. */
  if (splash_on) {
    for (int k = 0; k < 30; k++) {
      efi_splash_tick();
      gBS->Stall(16000);
    }
    efi_splash_done();
  }

  /* ---------------------------------------------------------------------
   * 7. Memmap para pasar al kernel.
   * --------------------------------------------------------------------- */
  status = get_memory_map(image_handle);
  if (EFI_ERROR(status)) {
    efi_print(L"[BOOT] Fallo obteniendo memmap\n");
    return status;
  }

  /* ---------------------------------------------------------------------
   * 8. Rellenar kernel_boot_info.
   * --------------------------------------------------------------------- */
  struct kernel_boot_info kinfo;
  zero_mem(&kinfo, sizeof(kinfo));
  if (gop) {
    kinfo.fb_base = gop->Mode->FrameBufferBase;
    kinfo.fb_size = gop->Mode->FrameBufferSize;
    kinfo.fb_width = gop->Mode->Info->HorizontalResolution;
    kinfo.fb_height = gop->Mode->Info->VerticalResolution;
    kinfo.fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    kinfo.fb_bpp = 32;
  }
  kinfo.edid_len = g_edid_len;
  copy_mem(kinfo.edid, g_edid, sizeof(g_edid));
  kinfo.memmap_size = mem_map.map_size;
  kinfo.memmap_desc_size = mem_map.desc_size;
  kinfo.memmap_desc_ver = mem_map.desc_version;
  kinfo.initrd_base = (uint64_t)initrd_phys;
  kinfo.initrd_size = (uint64_t)initrd_size;

  find_acpi_rsdp(system_table, kinfo.acpi_rsdp);
  if (cfg.have_cmdline)
    copy_mem(kinfo.cmdline, cfg.cmdline,
             CMDLINE_MAX < sizeof(kinfo.cmdline) ? CMDLINE_MAX
                                                 : sizeof(kinfo.cmdline));
  kinfo.cmdline[sizeof(kinfo.cmdline) - 1] = 0;

  /* ---------------------------------------------------------------------
   * 9. Saltar al kernel. No retorna.
   * --------------------------------------------------------------------- */
  jump_to_kernel(kernel_entry, image_handle, &kinfo, pml4_addr);

  while (1)
    __asm__ volatile("hlt");
}