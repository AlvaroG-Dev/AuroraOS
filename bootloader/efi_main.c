// bootloader/efi_main.c
// UEFI Bootloader para Aurora OS

#include <efi.h>
#include <efilib.h>
#include <stdint.h>

// [2.1] Ruta por defecto. Si /etc/aurora.conf no existe o no define
// kernel=, se usa esta. Si define una ruta que no existe, se avisa y se
// cae a esta (opción C del diseño).
#define KERNEL_PATH_DEFAULT L"\\kernel.elf"
#define CONFIG_PATH L"\\etc\\aurora.conf"
#define CONFIG_MAX_SIZE 4096
#define KERNEL_PATH_MAX 256
#define ET_EXEC 2

// Definiciones ELF64
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

// Info pasada al kernel (MISMO ORDEN Y TIPOS QUE EL KERNEL)
struct __attribute__((packed)) kernel_boot_info {
  uint64_t fb_base;
  uint64_t fb_size;
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_pitch;
  uint32_t fb_bpp;
  uint64_t memmap;
  uint64_t memmap_size;
  uint64_t memmap_desc_size;
  uint32_t memmap_desc_ver;
  uint8_t acpi_rsdp[64];
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
static EFI_LOADED_IMAGE *loaded_image = NULL;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;

static EFI_STATUS get_memory_map(EFI_HANDLE image_handle) {
  EFI_STATUS status;
  mem_map.map_size = 0;
  status = uefi_call_wrapper(BS->GetMemoryMap, 5, &mem_map.map_size, NULL,
                             &mem_map.map_key, &mem_map.desc_size,
                             &mem_map.desc_version);
  if (status != EFI_BUFFER_TOO_SMALL)
    return status;

  mem_map.map_size += 4 * mem_map.desc_size;
  status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                             mem_map.map_size, (VOID **)&mem_map.map);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(BS->GetMemoryMap, 5, &mem_map.map_size,
                             mem_map.map, &mem_map.map_key, &mem_map.desc_size,
                             &mem_map.desc_version);
  return status;
}

static EFI_STATUS map_page_4k(UINT64 *pml4, EFI_PHYSICAL_ADDRESS virt,
                              EFI_PHYSICAL_ADDRESS phys) {
  UINTN pml4_idx = (virt >> 39) & 0x1FF;
  UINTN pdpt_idx = (virt >> 30) & 0x1FF;
  UINTN pd_idx = (virt >> 21) & 0x1FF;
  UINTN pt_idx = (virt >> 12) & 0x1FF;

  if (!(pml4[pml4_idx] & 1)) {
    EFI_PHYSICAL_ADDRESS new_pdpt;
    uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1,
                      &new_pdpt);
    ZeroMem((VOID *)new_pdpt, 4096);
    pml4[pml4_idx] = new_pdpt | 0x03;
  }
  UINT64 *pdpt = (UINT64 *)(pml4[pml4_idx] & ~0xFFFULL);

  if (!(pdpt[pdpt_idx] & 1)) {
    EFI_PHYSICAL_ADDRESS new_pd;
    uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1,
                      &new_pd);
    ZeroMem((VOID *)new_pd, 4096);
    pdpt[pdpt_idx] = new_pd | 0x03;
  }
  UINT64 *pd = (UINT64 *)(pdpt[pdpt_idx] & ~0xFFFULL);

  if (!(pd[pd_idx] & 1)) {
    EFI_PHYSICAL_ADDRESS new_pt;
    uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1,
                      &new_pt);
    ZeroMem((VOID *)new_pt, 4096);
    pd[pd_idx] = new_pt | 0x03;
  }
  UINT64 *pt = (UINT64 *)(pd[pd_idx] & ~0xFFFULL);

  pt[pt_idx] = phys | 0x03;
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// [2.1] Parser del boot config.
//
// Formato (key=value, INI-lite):
//   # comentario
//   kernel=\boot\kernel.elf
//
// Reglas:
//   - Whitespace al inicio/fin de línea y de valor se recorta.
//   - Líneas vacías o que empiezan por '#' se ignoran.
//   - Claves desconocidas se ignoran (permite extender el formato).
//   - Se normaliza '/' a '\' (UEFI trabaja con backslashes).
//   - Si la ruta no empieza por '\', se prefija (rutas absolutas en ESP).
//
// Todo con arrays estáticos: el bootloader corre sin heap cómoda y no
// queremos fragmentar antes de ExitBootServices.
// ---------------------------------------------------------------------------

// Convierte ASCII a CHAR16 (los valores del .conf son ASCII).
static VOID ascii_to_utf16(const CHAR8 *src, UINTN src_len, CHAR16 *dst,
                           UINTN dst_max) {
  UINTN i = 0;
  while (i < src_len && i + 1 < dst_max) {
    CHAR8 c = src[i];
    if (c == '/')
      c = '\\';
    dst[i] = (CHAR16)c;
    i++;
  }
  dst[i] = 0;
}

// Busca "kernel=<ruta>" en el buffer. Devuelve TRUE si encuentra la clave.
static BOOLEAN parse_kernel_key(const CHAR8 *buf, UINTN size, CHAR16 *out,
                                UINTN out_max) {
  static const CHAR8 want[] = "kernel";
  const UINTN want_len = 6;
  UINTN i = 0;

  while (i < size) {
    UINTN line_start = i;
    while (i < size && buf[i] != '\n')
      i++;
    UINTN line_end = i;
    if (i < size)
      i++;

    // Recortar '\r' al final de línea.
    while (line_end > line_start && buf[line_end - 1] == '\r')
      line_end--;

    // Recortar whitespace al inicio.
    while (line_start < line_end &&
           (buf[line_start] == ' ' || buf[line_start] == '\t'))
      line_start++;

    if (line_start >= line_end)
      continue; // línea vacía
    if (buf[line_start] == '#')
      continue; // comentario

    // Buscar '='.
    UINTN eq = line_start;
    while (eq < line_end && buf[eq] != '=')
      eq++;
    if (eq >= line_end)
      continue; // sin '=', se ignora

    // Recortar la clave (antes del '=').
    UINTN key_end = eq;
    while (key_end > line_start &&
           (buf[key_end - 1] == ' ' || buf[key_end - 1] == '\t'))
      key_end--;
    UINTN key_len = key_end - line_start;
    if (key_len != want_len)
      continue;
    BOOLEAN key_match = TRUE;
    for (UINTN k = 0; k < want_len; k++) {
      if (buf[line_start + k] != want[k]) {
        key_match = FALSE;
        break;
      }
    }
    if (!key_match)
      continue;

    // Recortar el valor (después del '=').
    UINTN val_start = eq + 1;
    while (val_start < line_end &&
           (buf[val_start] == ' ' || buf[val_start] == '\t'))
      val_start++;
    UINTN val_end = line_end;
    while (val_end > val_start &&
           (buf[val_end - 1] == ' ' || buf[val_end - 1] == '\t'))
      val_end--;
    if (val_start >= val_end)
      continue; // valor vacío

    // Prefijar '\' si la ruta no es absoluta.
    UINTN oi = 0;
    if (buf[val_start] != '\\' && buf[val_start] != '/') {
      if (oi + 1 < out_max)
        out[oi++] = '\\';
    }
    ascii_to_utf16(buf + val_start, val_end - val_start, out + oi,
                   out_max - oi);
    return TRUE;
  }

  return FALSE;
}

// Lee /etc/aurora.conf del ESP y rellena out_path con la ruta del kernel.
// Si no existe el fichero, deja out_path con la ruta por defecto.
// Si el fichero existe pero no define kernel=, igual.
static VOID load_boot_config(EFI_FILE *root, CHAR16 *out_path, UINTN out_max) {
  // Rellenar con el default primero.
  UINTN i = 0;
  while (KERNEL_PATH_DEFAULT[i] && i + 1 < out_max) {
    out_path[i] = KERNEL_PATH_DEFAULT[i];
    i++;
  }
  out_path[i] = 0;

  EFI_FILE *file = NULL;
  EFI_STATUS status = uefi_call_wrapper(root->Open, 5, root, &file, CONFIG_PATH,
                                        EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status) || !file) {
    Print(L"[BOOT] %s no existe, usando %s\n", CONFIG_PATH,
          KERNEL_PATH_DEFAULT);
    return;
  }

  // Buffer estático: 4 KB es sobra para este fichero.
  static CHAR8 buf[CONFIG_MAX_SIZE];
  UINTN size = sizeof(buf) - 1;
  status = uefi_call_wrapper(file->Read, 3, file, &size, buf);
  uefi_call_wrapper(file->Close, 1, file);
  if (EFI_ERROR(status) || size == 0) {
    Print(L"[BOOT] WARN: %s vacío o ilegible, usando %s\n", CONFIG_PATH,
          KERNEL_PATH_DEFAULT);
    return;
  }
  buf[size] = 0;

  CHAR16 parsed[KERNEL_PATH_MAX];
  if (parse_kernel_key(buf, size, parsed, KERNEL_PATH_MAX)) {
    UINTN j = 0;
    while (parsed[j] && j + 1 < out_max) {
      out_path[j] = parsed[j];
      j++;
    }
    out_path[j] = 0;
    Print(L"[BOOT] Config: kernel=%s\n", out_path);
  } else {
    Print(L"[BOOT] Config: sin clave 'kernel', usando %s\n",
          KERNEL_PATH_DEFAULT);
  }
}

static EFI_STATUS load_kernel(EFI_FILE *root, const CHAR16 *kernel_path,
                              VOID **entry_point, UINT64 *pml4) {
  EFI_FILE *file = NULL;
  EFI_STATUS status = uefi_call_wrapper(
      root->Open, 5, root, &file, (CHAR16 *)kernel_path, EFI_FILE_MODE_READ, 0);

  if (EFI_ERROR(status)) {
    Print(L"[BOOT] Error abriendo %s: %r\n", kernel_path, status);
    return status;
  }

  struct Elf64_Ehdr ehdr;
  UINTN size = sizeof(ehdr);
  status = uefi_call_wrapper(file->Read, 3, file, &size, &ehdr);
  if (EFI_ERROR(status) || size != sizeof(ehdr))
    goto cleanup;

  if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
      ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
    Print(L"[BOOT] No es un ELF valido\n");
    status = EFI_INVALID_PARAMETER;
    goto cleanup;
  }
  if (ehdr.e_machine != EM_X86_64) {
    Print(L"[BOOT] ELF no es x86_64 (machine=%d)\n", ehdr.e_machine);
    status = EFI_UNSUPPORTED;
    goto cleanup;
  }
  if (ehdr.e_type != ET_EXEC) {
    Print(L"[BOOT] ELF no es ET_EXEC (type=%d)\n", ehdr.e_type);
    status = EFI_UNSUPPORTED;
    goto cleanup;
  }

  for (UINTN i = 0; i < ehdr.e_phnum; i++) {
    struct Elf64_Phdr phdr;
    UINTN phdr_size = sizeof(phdr);
    status = uefi_call_wrapper(file->SetPosition, 2, file,
                               ehdr.e_phoff + i * ehdr.e_phentsize);
    if (EFI_ERROR(status))
      goto cleanup;
    status = uefi_call_wrapper(file->Read, 3, file, &phdr_size, &phdr);
    if (EFI_ERROR(status))
      goto cleanup;

    if (phdr.p_type != PT_LOAD)
      continue;

    EFI_PHYSICAL_ADDRESS paddr = phdr.p_paddr;
    if (paddr == 0) {
      status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                                 EfiLoaderData, (phdr.p_memsz + 0xFFF) / 0x1000,
                                 &paddr);
    } else {
      UINTN num_pages = (phdr.p_memsz + 0xFFF) / 0x1000;
      if (num_pages == 0)
        num_pages = 1;
      status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress,
                                 EfiLoaderData, num_pages, &paddr);
      if (EFI_ERROR(status)) {
        status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                                   EfiLoaderData, num_pages, &paddr);
      }
    }
    if (EFI_ERROR(status)) {
      Print(L"[BOOT] Fallo alocando memoria para segmento %d\n", i);
      goto cleanup;
    }

    status = uefi_call_wrapper(file->SetPosition, 2, file, phdr.p_offset);
    if (EFI_ERROR(status))
      goto cleanup;
    UINTN read_size = phdr.p_filesz;
    status = uefi_call_wrapper(file->Read, 3, file, &read_size, (VOID *)paddr);
    if (EFI_ERROR(status))
      goto cleanup;

    if (phdr.p_memsz > phdr.p_filesz) {
      ZeroMem((VOID *)(paddr + phdr.p_filesz), phdr.p_memsz - phdr.p_filesz);
    }

    UINTN num_pages = (phdr.p_memsz + 0xFFF) / 0x1000;
    for (UINTN p = 0; p < num_pages; p++) {
      map_page_4k(pml4, phdr.p_vaddr + p * 0x1000, paddr + p * 0x1000);
    }

    Print(L"[BOOT] Segmento %d: vaddr=0x%lx paddr=0x%lx pages=%d\n", i,
          phdr.p_vaddr, paddr, num_pages);
  }

  *entry_point = (VOID *)ehdr.e_entry;
  Print(L"[BOOT] Entry point: 0x%lx\n", ehdr.e_entry);
  status = EFI_SUCCESS;

cleanup:
  uefi_call_wrapper(file->Close, 1, file);
  return status;
}

static EFI_STATUS setup_framebuffer(void) {
  EFI_STATUS status =
      uefi_call_wrapper(BS->LocateProtocol, 3, &gEfiGraphicsOutputProtocolGuid,
                        NULL, (VOID **)&gop);
  if (EFI_ERROR(status) || !gop) {
    Print(L"[BOOT] GOP no disponible\n");
    return status;
  }
  Print(L"[BOOT] FB: %dx%d @ 0x%lx size=0x%lx pitch=%d\n",
        gop->Mode->Info->HorizontalResolution,
        gop->Mode->Info->VerticalResolution, gop->Mode->FrameBufferBase,
        gop->Mode->FrameBufferSize, gop->Mode->Info->PixelsPerScanLine * 4);
  return EFI_SUCCESS;
}

static EFI_STATUS build_page_tables(EFI_PHYSICAL_ADDRESS *pml4_out) {
  EFI_STATUS status;
  EFI_PHYSICAL_ADDRESS pml4_addr = 0xFFFFFFFF;
  EFI_PHYSICAL_ADDRESS pdpt_addr = 0xFFFFFFFF;
  EFI_PHYSICAL_ADDRESS pd_addrs[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                      0xFFFFFFFF};

  status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress,
                             EfiLoaderData, 1, &pml4_addr);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress,
                             EfiLoaderData, 1, &pdpt_addr);
  if (EFI_ERROR(status))
    return status;

  for (int i = 0; i < 4; i++) {
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress,
                               EfiLoaderData, 1, &pd_addrs[i]);
    if (EFI_ERROR(status))
      return status;
  }

  UINT64 *pml4 = (UINT64 *)pml4_addr;
  UINT64 *pdpt = (UINT64 *)pdpt_addr;

  ZeroMem(pml4, 4096);
  ZeroMem(pdpt, 4096);
  for (int i = 0; i < 4; i++)
    ZeroMem((VOID *)pd_addrs[i], 4096);

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

// ---------------------------------------------------------------------------
// ACPI: buscar el RSDP y copiarlo por valor al kinfo.
//
// Se llama ANTES de ExitBootServices, así que el firmware está vivo y las
// tablas ACPI son accesibles. El kernel recibe los 64 bytes directamente
// en kernel_boot_info.acpi_rsdp. No hay dependencia de direcciones físicas
// del firmware.
// ---------------------------------------------------------------------------
// Compara dos GUIDs byte a byte.
static BOOLEAN guid_equals(const EFI_GUID *a, const EFI_GUID *b) {
  return (BOOLEAN)(CompareMem((VOID *)a, (VOID *)b, sizeof(EFI_GUID)) == 0);
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

  // Buscar primero ACPI 2.0+ (preferido), luego ACPI 1.0.
  const EFI_GUID *guids_to_try[2] = {&acpi20_guid, &acpi10_guid};

  for (int g = 0; g < 2; g++) {
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
      EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
      if (!guid_equals(&ct->VendorGuid, guids_to_try[g]))
        continue;

      uint8_t *rsdp = (uint8_t *)ct->VendorTable;
      if (!rsdp)
        continue;

      // Verificar firma "RSD PTR " por si acaso.
      if (rsdp[0] != 'R' || rsdp[1] != 'S' || rsdp[2] != 'D' ||
          rsdp[3] != ' ' || rsdp[4] != 'P' || rsdp[5] != 'T' ||
          rsdp[6] != 'R' || rsdp[7] != ' ') {
        Print(L"[BOOT] ACPI RSDP con GUID correcto pero firma invalida\n");
        continue;
      }

      for (int j = 0; j < 64; j++)
        out_rsdp[j] = rsdp[j];

      Print(L"[BOOT] ACPI RSDP encontrado (%s), revision=%d\n",
            g == 0 ? L"ACPI 2.0+" : L"ACPI 1.0", rsdp[15]);
      return;
    }
  }
  Print(L"[BOOT] ACPI RSDP no encontrado\n");
}

static void jump_to_kernel(VOID *kernel_entry, EFI_HANDLE image_handle,
                           struct kernel_boot_info *kinfo,
                           EFI_PHYSICAL_ADDRESS pml4_addr) {
  EFI_STATUS status;

  EFI_PHYSICAL_ADDRESS safe_memmap_addr = 0xFFFFFFFF;
  status = uefi_call_wrapper(
      BS->AllocatePages, 4, AllocateMaxAddress, EfiLoaderData,
      (mem_map.map_size + 0xFFF + 2048) / 0x1000, &safe_memmap_addr);
  if (EFI_ERROR(status)) {
    Print(L"[BOOT] Fallo alocando buffer seguro para memmap\n");
    return;
  }

  Print(L"[BOOT] Salida de Boot Services...\n");

  get_memory_map(image_handle);
  CopyMem((VOID *)safe_memmap_addr, mem_map.map, mem_map.map_size);
  kinfo->memmap = safe_memmap_addr;

  __asm__ volatile("cli");

  status =
      uefi_call_wrapper(BS->ExitBootServices, 2, image_handle, mem_map.map_key);
  if (EFI_ERROR(status)) {
    get_memory_map(image_handle);
    CopyMem((VOID *)safe_memmap_addr, mem_map.map, mem_map.map_size);
    kinfo->memmap = safe_memmap_addr;
    status = uefi_call_wrapper(BS->ExitBootServices, 2, image_handle,
                               mem_map.map_key);
  }

  __asm__ volatile("movq %0, %%cr3" : : "r"(pml4_addr) : "memory");

  typedef void (*kernel_fn_t)(struct kernel_boot_info *)
      __attribute__((sysv_abi));
  kernel_fn_t kmain = (kernel_fn_t)kernel_entry;

  kmain(kinfo);

  while (1) {
    __asm__ volatile("hlt");
  }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle,
                           EFI_SYSTEM_TABLE *system_table) {
  InitializeLib(image_handle, system_table);
  Print(L"\n=== AURORA OS BOOTLOADER ===\n");
  Print(L"[BOOT] Cargando kernel...\n");

  EFI_STATUS status =
      uefi_call_wrapper(BS->OpenProtocol, 6, image_handle, &LoadedImageProtocol,
                        (VOID **)&loaded_image, image_handle, NULL,
                        EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  EFI_FILE *root = NULL;
  if (EFI_ERROR(status)) {
    Print(L"[BOOT] Error LoadedImageProtocol\n");
    return status;
  }

  status =
      uefi_call_wrapper(BS->OpenProtocol, 6, loaded_image->DeviceHandle,
                        &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs,
                        image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR(status)) {
    Print(L"[BOOT] Error SimpleFileSystemProtocol\n");
    return status;
  }

  status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
  if (EFI_ERROR(status)) {
    Print(L"[BOOT] Error abriendo volumen raiz\n");
    return status;
  }

  setup_framebuffer();

  EFI_PHYSICAL_ADDRESS pml4_addr = 0;
  status = build_page_tables(&pml4_addr);
  if (EFI_ERROR(status)) {
    Print(L"[BOOT] Fallo construyendo tablas de pagina\n");
    return status;
  }

  // [2.1] Leer /etc/aurora.conf (si existe). Fallback: KERNEL_PATH_DEFAULT.
  CHAR16 kernel_path[KERNEL_PATH_MAX];
  load_boot_config(root, kernel_path, KERNEL_PATH_MAX);

  VOID *kernel_entry = NULL;
  status = load_kernel(root, kernel_path, &kernel_entry, (UINT64 *)pml4_addr);
  if (EFI_ERROR(status)) {
    // Opción C: si la ruta era custom, intentar el default con warning.
    BOOLEAN is_default = (StrCmp(kernel_path, KERNEL_PATH_DEFAULT) == 0);
    if (!is_default) {
      Print(L"[BOOT] WARN: %s no existe, probando %s\n", kernel_path,
            KERNEL_PATH_DEFAULT);
      status = load_kernel(root, KERNEL_PATH_DEFAULT, &kernel_entry,
                           (UINT64 *)pml4_addr);
    }
    if (EFI_ERROR(status)) {
      Print(L"[BOOT] Fallo al cargar kernel\n");
      return status;
    }
  }

  status = get_memory_map(image_handle);
  if (EFI_ERROR(status)) {
    Print(L"[BOOT] Fallo obteniendo memmap\n");
    return status;
  }

  struct kernel_boot_info kinfo = {0};
  if (gop) {
    kinfo.fb_base = gop->Mode->FrameBufferBase;
    kinfo.fb_size = gop->Mode->FrameBufferSize;
    kinfo.fb_width = gop->Mode->Info->HorizontalResolution;
    kinfo.fb_height = gop->Mode->Info->VerticalResolution;
    kinfo.fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    kinfo.fb_bpp = 32;
  }
  kinfo.memmap_size = mem_map.map_size;
  kinfo.memmap_desc_size = mem_map.desc_size;
  kinfo.memmap_desc_ver = mem_map.desc_version;

  // Buscar el RSDP y copiarlo al kinfo por valor.
  // Debe llamarse ANTES de ExitBootServices (que se hace dentro de
  // jump_to_kernel).
  find_acpi_rsdp(system_table, kinfo.acpi_rsdp);

  jump_to_kernel(kernel_entry, image_handle, &kinfo, pml4_addr);

  while (1)
    __asm__ volatile("hlt");
}