// bootloader/efi_main.c
// UEFI Bootloader para Aurora OS — versión sin gnu-efi.
// Todo el código se apoya únicamente en efi_min.h.

#include "efi_min.h"
#include <stdarg.h>

/* ---- Globals ---- */
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_HANDLE gImageHandle = NULL;

/* ---- GUIDs ---- */
const EFI_GUID gEfiLoadedImageProtocolGuid = {
    0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
const EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
const EFI_GUID gEfiGraphicsOutputProtocolGuid = {
    0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}
};

/* ---- Helpers de memoria/strings ---- */
void zero_mem(VOID *dst, UINTN n) {
    UINT8 *p = (UINT8 *)dst;
    while (n--) *p++ = 0;
}
void copy_mem(VOID *dst, const VOID *src, UINTN n) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    while (n--) *d++ = *s++;
}
int compare_mem(const VOID *a, const VOID *b, UINTN n) {
    const UINT8 *pa = (const UINT8 *)a, *pb = (const UINT8 *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}
int str_cmp_16(const CHAR16 *a, const CHAR16 *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(*a - *b);
}

/* ---- efi_print: subset de gnu-efi Print ----
 * Soporta: %s (CHAR16*), %d (int/INT64), %lx (UINT64 hex),
 *          %x (UINT32 hex), %r (EFI_STATUS como 0x...), %% */
static CHAR16 *p_u64_dec(CHAR16 *out, UINT64 v) {
    CHAR16 tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = L'0';
    while (v) { tmp[n++] = (CHAR16)(L'0' + (v % 10)); v /= 10; }
    while (n > 0) *out++ = tmp[--n];
    return out;
}
static CHAR16 *p_u64_hex(CHAR16 *out, UINT64 v) {
    CHAR16 tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = L'0';
    while (v) {
        int d = (int)(v & 0xF);
        tmp[n++] = (d < 10) ? (CHAR16)(L'0' + d) : (CHAR16)(L'a' + d - 10);
        v >>= 4;
    }
    while (n > 0) *out++ = tmp[--n];
    return out;
}

void efi_print(const CHAR16 *fmt, ...) {
    static CHAR16 buf[1024];
    CHAR16 *out = buf;
    CHAR16 *end = buf + 1022;

    va_list ap;
    va_start(ap, fmt);

    while (*fmt && out < end) {
        if (*fmt != L'%') { *out++ = *fmt++; continue; }
        fmt++;
        if (out + 32 >= end) break;

        switch (*fmt) {
        case L's': {
            const CHAR16 *s = va_arg(ap, const CHAR16 *);
            if (!s) s = L"(null)";
            while (*s && out < end) *out++ = *s++;
            break;
        }
        case L'd':
        case L'i': {
            INT64 v = va_arg(ap, INT64);
            if (v < 0) { *out++ = L'-'; v = -v; }
            out = p_u64_dec(out, (UINT64)v);
            break;
        }
        case L'l': {
            CHAR16 c = *(fmt + 1);
            if (c == L'x' || c == L'X') {
                fmt++;
                out = p_u64_hex(out, va_arg(ap, UINT64));
            } else if (c == L'u') {
                fmt++;
                out = p_u64_dec(out, va_arg(ap, UINT64));
            } else {
                *out++ = L'?';
            }
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
            *out++ = L'0'; *out++ = L'x';
            out = p_u64_hex(out, v);
            break;
        }
        case L'%':
            *out++ = L'%';
            break;
        default:
            *out++ = L'?';
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
#define KERNEL_PATH_DEFAULT L"\\kernel.elf"
#define CONFIG_PATH         L"\\etc\\aurora.conf"
#define CONFIG_MAX_SIZE     4096
#define KERNEL_PATH_MAX     256
#define ET_EXEC             2

/* ---- ELF64 ---- */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
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
    Elf64_Off  e_phoff;
    Elf64_Off  e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

struct Elf64_Phdr {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

/* ---- kernel_boot_info (mismo layout que el kernel) ---- */
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
    uint8_t  acpi_rsdp[64];
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
static EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;

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
    status = gBS->AllocatePool(EfiLoaderData, mem_map.map_size,
                               (VOID **)&mem_map.map);
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
    UINTN pd_idx   = (virt >> 21) & 0x1FF;
    UINTN pt_idx   = (virt >> 12) & 0x1FF;

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

/* ---- Parser del .conf ---- */
static VOID ascii_to_utf16(const CHAR8 *src, UINTN src_len, CHAR16 *dst,
                           UINTN dst_max) {
    UINTN i = 0;
    while (i < src_len && i + 1 < dst_max) {
        CHAR8 c = src[i];
        if (c == '/') c = '\\';
        dst[i] = (CHAR16)c;
        i++;
    }
    dst[i] = 0;
}

static BOOLEAN parse_kernel_key(const CHAR8 *buf, UINTN size, CHAR16 *out,
                                UINTN out_max) {
    static const CHAR8 want[] = "kernel";
    const UINTN want_len = 6;
    UINTN i = 0;

    while (i < size) {
        UINTN line_start = i;
        while (i < size && buf[i] != '\n') i++;
        UINTN line_end = i;
        if (i < size) i++;

        while (line_end > line_start && buf[line_end - 1] == '\r') line_end--;
        while (line_start < line_end &&
               (buf[line_start] == ' ' || buf[line_start] == '\t'))
            line_start++;

        if (line_start >= line_end) continue;
        if (buf[line_start] == '#') continue;

        UINTN eq = line_start;
        while (eq < line_end && buf[eq] != '=') eq++;
        if (eq >= line_end) continue;

        UINTN key_end = eq;
        while (key_end > line_start &&
               (buf[key_end - 1] == ' ' || buf[key_end - 1] == '\t'))
            key_end--;
        UINTN key_len = key_end - line_start;
        if (key_len != want_len) continue;

        BOOLEAN key_match = TRUE;
        for (UINTN k = 0; k < want_len; k++) {
            if (buf[line_start + k] != want[k]) { key_match = FALSE; break; }
        }
        if (!key_match) continue;

        UINTN val_start = eq + 1;
        while (val_start < line_end &&
               (buf[val_start] == ' ' || buf[val_start] == '\t'))
            val_start++;
        UINTN val_end = line_end;
        while (val_end > val_start &&
               (buf[val_end - 1] == ' ' || buf[val_end - 1] == '\t'))
            val_end--;
        if (val_start >= val_end) continue;

        UINTN oi = 0;
        if (buf[val_start] != '\\' && buf[val_start] != '/') {
            if (oi + 1 < out_max) out[oi++] = '\\';
        }
        ascii_to_utf16(buf + val_start, val_end - val_start, out + oi,
                       out_max - oi);
        return TRUE;
    }
    return FALSE;
}

static VOID load_boot_config(EFI_FILE *root, CHAR16 *out_path, UINTN out_max) {
    UINTN i = 0;
    while (KERNEL_PATH_DEFAULT[i] && i + 1 < out_max) {
        out_path[i] = KERNEL_PATH_DEFAULT[i];
        i++;
    }
    out_path[i] = 0;

    EFI_FILE *file = NULL;
    EFI_STATUS status = root->Open(root, &file, CONFIG_PATH,
                                   EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file) {
        efi_print(L"[BOOT] %s no existe, usando %s\n", CONFIG_PATH,
                  KERNEL_PATH_DEFAULT);
        return;
    }

    static CHAR8 buf[CONFIG_MAX_SIZE];
    UINTN size = sizeof(buf) - 1;
    status = file->Read(file, &size, buf);
    file->Close(file);
    if (EFI_ERROR(status) || size == 0) {
        efi_print(L"[BOOT] WARN: %s vacío o ilegible, usando %s\n", CONFIG_PATH,
                  KERNEL_PATH_DEFAULT);
        return;
    }
    buf[size] = 0;

    CHAR16 parsed[KERNEL_PATH_MAX];
    if (parse_kernel_key(buf, size, parsed, KERNEL_PATH_MAX)) {
        UINTN j = 0;
        while (parsed[j] && j + 1 < out_max) { out_path[j] = parsed[j]; j++; }
        out_path[j] = 0;
        efi_print(L"[BOOT] Config: kernel=%s\n", out_path);
    } else {
        efi_print(L"[BOOT] Config: sin clave 'kernel', usando %s\n",
                  KERNEL_PATH_DEFAULT);
    }
}

/* ---- Carga del kernel ---- */
static EFI_STATUS load_kernel(EFI_FILE *root, const CHAR16 *kernel_path,
                              VOID **entry_point, UINT64 *pml4) {
    EFI_FILE *file = NULL;
    EFI_STATUS status = root->Open(root, &file, (CHAR16 *)kernel_path,
                                   EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        efi_print(L"[BOOT] Error abriendo %s: %r\n", kernel_path, status);
        return status;
    }

    struct Elf64_Ehdr ehdr;
    UINTN size = sizeof(ehdr);
    status = file->Read(file, &size, &ehdr);
    if (EFI_ERROR(status) || size != sizeof(ehdr)) goto cleanup;

    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        efi_print(L"[BOOT] No es un ELF valido\n");
        status = EFI_INVALID_PARAMETER;
        goto cleanup;
    }
    if (ehdr.e_machine != EM_X86_64) {
        efi_print(L"[BOOT] ELF no es x86_64 (machine=%d)\n", ehdr.e_machine);
        status = EFI_UNSUPPORTED;
        goto cleanup;
    }
    if (ehdr.e_type != ET_EXEC) {
        efi_print(L"[BOOT] ELF no es ET_EXEC (type=%d)\n", ehdr.e_type);
        status = EFI_UNSUPPORTED;
        goto cleanup;
    }

    for (UINTN i = 0; i < ehdr.e_phnum; i++) {
        struct Elf64_Phdr phdr;
        UINTN phdr_size = sizeof(phdr);
        status = file->SetPosition(file, ehdr.e_phoff + i * ehdr.e_phentsize);
        if (EFI_ERROR(status)) goto cleanup;
        status = file->Read(file, &phdr_size, &phdr);
        if (EFI_ERROR(status)) goto cleanup;

        if (phdr.p_type != PT_LOAD) continue;

        EFI_PHYSICAL_ADDRESS paddr = phdr.p_paddr;
        if (paddr == 0) {
            status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                        (phdr.p_memsz + 0xFFF) / 0x1000,
                                        &paddr);
        } else {
            UINTN num_pages = (phdr.p_memsz + 0xFFF) / 0x1000;
            if (num_pages == 0) num_pages = 1;
            status = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                                        num_pages, &paddr);
            if (EFI_ERROR(status)) {
                status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                            num_pages, &paddr);
            }
        }
        if (EFI_ERROR(status)) {
            efi_print(L"[BOOT] Fallo alocando memoria para segmento %d\n", i);
            goto cleanup;
        }

        status = file->SetPosition(file, phdr.p_offset);
        if (EFI_ERROR(status)) goto cleanup;
        UINTN read_size = phdr.p_filesz;
        status = file->Read(file, &read_size, (VOID *)paddr);
        if (EFI_ERROR(status)) goto cleanup;

        if (phdr.p_memsz > phdr.p_filesz) {
            zero_mem((VOID *)(paddr + phdr.p_filesz),
                     phdr.p_memsz - phdr.p_filesz);
        }

        UINTN num_pages = (phdr.p_memsz + 0xFFF) / 0x1000;
        for (UINTN p = 0; p < num_pages; p++) {
            map_page_4k(pml4, phdr.p_vaddr + p * 0x1000, paddr + p * 0x1000);
        }

        efi_print(L"[BOOT] Segmento %d: vaddr=0x%lx paddr=0x%lx pages=%d\n",
                  i, phdr.p_vaddr, paddr, num_pages);
    }

    *entry_point = (VOID *)ehdr.e_entry;
    efi_print(L"[BOOT] Entry point: 0x%lx\n", ehdr.e_entry);
    status = EFI_SUCCESS;

cleanup:
    file->Close(file);
    return status;
}

/* ---- Framebuffer ---- */
static EFI_STATUS setup_framebuffer(void) {
    EFI_STATUS status = gBS->LocateProtocol(
        (EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || !gop) {
        efi_print(L"[BOOT] GOP no disponible\n");
        return status;
    }
    efi_print(L"[BOOT] FB: %dx%d @ 0x%lx size=0x%lx pitch=%d\n",
              gop->Mode->Info->HorizontalResolution,
              gop->Mode->Info->VerticalResolution,
              gop->Mode->FrameBufferBase,
              gop->Mode->FrameBufferSize,
              gop->Mode->Info->PixelsPerScanLine * 4);
    return EFI_SUCCESS;
}

/* ---- Tablas de página ---- */
static EFI_STATUS build_page_tables(EFI_PHYSICAL_ADDRESS *pml4_out) {
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS pml4_addr = 0xFFFFFFFF;
    EFI_PHYSICAL_ADDRESS pdpt_addr = 0xFFFFFFFF;
    EFI_PHYSICAL_ADDRESS pd_addrs[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                        0xFFFFFFFF};

    status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1,
                                &pml4_addr);
    if (EFI_ERROR(status)) return status;
    status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1,
                                &pdpt_addr);
    if (EFI_ERROR(status)) return status;
    for (int i = 0; i < 4; i++) {
        status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1,
                                    &pd_addrs[i]);
        if (EFI_ERROR(status)) return status;
    }

    UINT64 *pml4 = (UINT64 *)pml4_addr;
    UINT64 *pdpt = (UINT64 *)pdpt_addr;

    zero_mem(pml4, 4096);
    zero_mem(pdpt, 4096);
    for (int i = 0; i < 4; i++) zero_mem((VOID *)pd_addrs[i], 4096);

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

/* ---- ACPI RSDP ---- */
static BOOLEAN guid_equals(const EFI_GUID *a, const EFI_GUID *b) {
    return (BOOLEAN)(compare_mem(a, b, sizeof(EFI_GUID)) == 0);
}

static void find_acpi_rsdp(EFI_SYSTEM_TABLE *st, uint8_t *out_rsdp) {
    EFI_GUID acpi20_guid = {0x8868e871, 0xe4f1, 0x11d3,
                            {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
    EFI_GUID acpi10_guid = {0xeb9d2d30, 0x2d88, 0x11d3,
                            {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

    for (int j = 0; j < 64; j++) out_rsdp[j] = 0;

    const EFI_GUID *guids_to_try[2] = {&acpi20_guid, &acpi10_guid};

    for (int g = 0; g < 2; g++) {
        for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
            if (!guid_equals(&ct->VendorGuid, guids_to_try[g])) continue;

            uint8_t *rsdp = (uint8_t *)ct->VendorTable;
            if (!rsdp) continue;

            if (rsdp[0] != 'R' || rsdp[1] != 'S' || rsdp[2] != 'D' ||
                rsdp[3] != ' ' || rsdp[4] != 'P' || rsdp[5] != 'T' ||
                rsdp[6] != 'R' || rsdp[7] != ' ') {
                efi_print(L"[BOOT] ACPI RSDP con GUID correcto pero firma invalida\n");
                continue;
            }

            for (int j = 0; j < 64; j++) out_rsdp[j] = rsdp[j];

            efi_print(L"[BOOT] ACPI RSDP encontrado (%s), revision=%d\n",
                      g == 0 ? L"ACPI 2.0+" : L"ACPI 1.0", rsdp[15]);
            return;
        }
    }
    efi_print(L"[BOOT] ACPI RSDP no encontrado\n");
}

static void jump_to_kernel(VOID *kernel_entry, EFI_HANDLE image_handle,
    struct kernel_boot_info *kinfo,
    EFI_PHYSICAL_ADDRESS pml4_addr) {
EFI_STATUS status;

// ---------------------------------------------------------------------
// 0. Todo lo que se usa DESPUÉS de cambiar CR3 debe estar < 4 GiB (único
//    rango con identity map en las tablas del kernel). La imagen del
//    bootloader, con >4 GiB de RAM, puede estar por encima.
// ---------------------------------------------------------------------
// 0a. Trampoline ejecutable: mov cr3,rcx ; mov rdi,rdx ; jmp r8
EFI_PHYSICAL_ADDRESS tramp_addr = 0xFFFFFFFF;
status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderCode, 1,
       &tramp_addr);
if (EFI_ERROR(status)) {
efi_print(L"[BOOT] trampoline alloc FAIL %r\n", status);
return;
}
static const UINT8 tramp_code[] = {
0x0F, 0x22, 0xD9, // mov cr3, rcx
0x48, 0x89, 0xD7, // mov rdi, rdx
0x41, 0xFF, 0xE0  // jmp r8
};
copy_mem((VOID *)tramp_addr, tramp_code, sizeof(tramp_code));

// 0b. Copia de kinfo en una página baja (el original vive en la pila
//     UEFI, que normalmente está <4 GiB pero no está garantizado).
EFI_PHYSICAL_ADDRESS kinfo_addr = 0xFFFFFFFF;
status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1,
       &kinfo_addr);
if (EFI_ERROR(status)) {
efi_print(L"[BOOT] kinfo alloc FAIL %r\n", status);
return;
}
copy_mem((VOID *)kinfo_addr, kinfo, sizeof(*kinfo));
kinfo = (struct kernel_boot_info *)kinfo_addr;

// ---------------------------------------------------------------------
// 1. Buffer "seguro" para el memory map final.
// ---------------------------------------------------------------------
EFI_PHYSICAL_ADDRESS safe_memmap_addr = 0;
UINTN safe_pages = (mem_map.map_size + 0xFFF + 65536) / 0x1000;
if (safe_pages < 16) safe_pages = 16;
status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
       safe_pages, &safe_memmap_addr);
if (EFI_ERROR(status)) {
efi_print(L"[BOOT] safe_memmap alloc FAIL %r\n", status);
return;
}
efi_print(L"[BOOT] safe_memmap @ %lx (%lu pages)\n",
(UINT64)safe_memmap_addr, (UINT64)safe_pages);

// 2. IRQs off.
__asm__ volatile("cli");

// 3. Bucle de ExitBootServices (sin cambios).
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
EFI_STATUS s = gBS->GetMemoryMap(&ms, NULL, &map_key, &desc_size,
              &desc_ver);
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

efi_print(L"[BOOT] t%d ExitBootServices key=%lx sz=%lx FAIL %r\n",
(INT64)attempt, (UINT64)map_key, (UINT64)map_buf_size, status);
}

if (EFI_ERROR(status)) {
efi_print(L"[BOOT] ExitBootServices fallo tras 32 intentos. Halt.\n");
while (1)
__asm__ volatile("hlt");
}

// ---------------------------------------------------------------------
// 4. Cambiar CR3 y saltar al kernel DESDE el trampoline (identity-mapped).
//    Convención del bootloader (MS ABI): rcx=pml4, rdx=kinfo, r8=entry.
//    El trampoline deja kinfo en rdi (SysV) y salta a entry. No vuelve.
// ---------------------------------------------------------------------
typedef void (*__attribute__((ms_abi)) tramp_fn_t)(
UINT64 pml4, struct kernel_boot_info *kinfo, VOID *entry);
((tramp_fn_t)tramp_addr)((UINT64)pml4_addr, kinfo, kernel_entry);

while (1)
__asm__ volatile("hlt");
}

/* ---- Entry point ---- */
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    gImageHandle = image_handle;
    gST = system_table;
    gBS = system_table->BootServices;

    efi_print(L"\n=== AURORA OS BOOTLOADER ===\n");
    efi_print(L"[BOOT] Cargando kernel...\n");

    EFI_STATUS status = gBS->OpenProtocol(
        image_handle, (EFI_GUID *)&gEfiLoadedImageProtocolGuid,
        (VOID **)&loaded_image, image_handle, NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    EFI_FILE *root = NULL;
    if (EFI_ERROR(status)) {
        efi_print(L"[BOOT] Error LoadedImageProtocol\n");
        return status;
    }

    status = gBS->OpenProtocol(
        loaded_image->DeviceHandle,
        (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&fs, image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(status)) {
        efi_print(L"[BOOT] Error SimpleFileSystemProtocol\n");
        return status;
    }

    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        efi_print(L"[BOOT] Error abriendo volumen raiz\n");
        return status;
    }

    setup_framebuffer();

    EFI_PHYSICAL_ADDRESS pml4_addr = 0;
    status = build_page_tables(&pml4_addr);
    if (EFI_ERROR(status)) {
        efi_print(L"[BOOT] Fallo construyendo tablas de pagina\n");
        return status;
    }

    CHAR16 kernel_path[KERNEL_PATH_MAX];
    load_boot_config(root, kernel_path, KERNEL_PATH_MAX);

    VOID *kernel_entry = NULL;
    status = load_kernel(root, kernel_path, &kernel_entry, (UINT64 *)pml4_addr);
    if (EFI_ERROR(status)) {
        BOOLEAN is_default = (str_cmp_16(kernel_path, KERNEL_PATH_DEFAULT) == 0);
        if (!is_default) {
            efi_print(L"[BOOT] WARN: %s no existe, probando %s\n",
                      kernel_path, KERNEL_PATH_DEFAULT);
            status = load_kernel(root, KERNEL_PATH_DEFAULT, &kernel_entry,
                                 (UINT64 *)pml4_addr);
        }
        if (EFI_ERROR(status)) {
            efi_print(L"[BOOT] Fallo al cargar kernel\n");
            return status;
        }
    }

    status = get_memory_map(image_handle);
    if (EFI_ERROR(status)) {
        efi_print(L"[BOOT] Fallo obteniendo memmap\n");
        return status;
    }

    struct kernel_boot_info kinfo = {0};
    if (gop) {
        kinfo.fb_base   = gop->Mode->FrameBufferBase;
        kinfo.fb_size   = gop->Mode->FrameBufferSize;
        kinfo.fb_width  = gop->Mode->Info->HorizontalResolution;
        kinfo.fb_height = gop->Mode->Info->VerticalResolution;
        kinfo.fb_pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
        kinfo.fb_bpp    = 32;
    }
    kinfo.memmap_size      = mem_map.map_size;
    kinfo.memmap_desc_size = mem_map.desc_size;
    kinfo.memmap_desc_ver  = mem_map.desc_version;

    find_acpi_rsdp(system_table, kinfo.acpi_rsdp);

    jump_to_kernel(kernel_entry, image_handle, &kinfo, pml4_addr);

    while (1) __asm__ volatile("hlt");
}