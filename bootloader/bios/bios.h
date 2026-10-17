/* bootloader/bios/bios.h — cabecera comun de todo el bootloader BIOS. */
#ifndef AURORA_BIOS_H
#define AURORA_BIOS_H

#include "kernel_boot_info.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ==========================================================================
 * Constantes de layout de memoria del bootloader
 * ========================================================================== */
#define STAGE1_ADDR 0x7C00u
#define STAGE2_ADDR 0x7E00u
#define STAGE2_MAX_SIZE 0x7C00u /* 31 KB hasta el hueco siguiente */

/* Bump allocator y scratch */
#define BOOT_STACK_TOP 0x9F000u /* top de stack para C de 32 bits */
/* Bounce buffer para int 13h.
 * [FIX] Movido a 0x1000 porque muchas BIOS de portátiles solo pueden
 * hacer DMA en los primeros 64 KB de RAM. */
#define BOUNCE_BUF_ADDR 0x1000u
#define BOUNCE_BUF_SIZE 0x6000u /* 24 KB, llega hasta 0x7000 (seguro) */
#define BOUNCE_MAX_SECTORS 48   /* 48 * 512 = 24576 bytes */

/* Estructuras en low memory (< 1 MB) que sobreviven al long mode */
#define E820_BUF_ADDR 0x20000u /* 128 entradas de 24 B = 3 KB */
#define E820_MAX_ENTRIES 128
#define VBE_INFO_ADDR 0x21000u
#define VBE_MODE_INFO_ADDR 0x22000u
#define PML4_ADDR 0x40000u /* PML4 inicial (identity 0..4 GiB) */
#define PML4_SIZE 0x1000
#define BOOT_PARAMS_ADDR 0x23000u     /* boot_params_phys_t persistente */
#define KERNEL_STAGE_ADDR 0x01000000u /* 16 MB: staging de kernel.elf */
#define KERNEL_STAGE_MAX (16u * 1024u * 1024u)
#define INITRD_MAX_SCAN_MB 4096u /* nunca buscamos initrd > 4 GB */

/* Tipos EFI (solo los que el kernel reconoce via type==7) */
#define EFI_RESERVED_MEMORY_TYPE 0
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_RUNTIME_SERVICES_CODE 5
#define EFI_RUNTIME_SERVICES_DATA 6
#define EFI_CONVENTIONAL_MEMORY 7
#define EFI_UNUSABLE_MEMORY 8
#define EFI_ACPI_RECLAIM_MEMORY 9
#define EFI_ACPI_MEMORY_NVS 10
#define EFI_MEMORY_MAPPED_IO 11
#define EFI_MEMORY_MAPPED_IO_PORT 12
#define EFI_PAL_CODE 13
#define EFI_PERSISTENT_MEMORY 14

/* ==========================================================================
 * Tipos compartidos
 * ========================================================================== */
typedef struct {
  uint32_t type;
  uint32_t pad;
  uint64_t phys_start;
  uint64_t virt_start;
  uint64_t num_pages;
  uint64_t attr;
} __attribute__((packed)) efi_memory_descriptor_t;

_Static_assert(sizeof(efi_memory_descriptor_t) == 40,
               "efi_memory_descriptor_t debe ser 40 bytes");

/* E820 crudo: viene del stage2.asm */
typedef struct {
  uint64_t base;
  uint64_t length;
  uint32_t type;
  uint32_t acpi_ext;
} __attribute__((packed)) e820_entry_t;

/* VBE: rellena stage2.asm, leida por bios_main */
typedef struct {
  uint32_t lfb_phys;
  uint32_t pitch;
  uint32_t width;
  uint32_t height;
  uint32_t bpp;
  uint16_t mode;
  uint8_t ok;
  uint8_t _pad[3];
} __attribute__((packed)) vbe_desc_t;

/* boot_params: escrito por bios_main antes de saltar a long mode */
typedef struct {
  uint32_t magic; /* 0xAUR0 */
  uint32_t e820_count;
  uint64_t kernel_entry;
  uint64_t pml4_phys;
  uint64_t memmap_phys;
  uint64_t memmap_size;
  uint64_t initrd_phys;
  uint64_t initrd_size;
  struct kernel_boot_info kinfo;
} __attribute__((packed)) boot_params_t;

/* Config */
typedef struct {
  char kernel_path[128];
  char initrd_path[128];
  char cmdline[256];
  int have_kernel;
  int have_initrd;
  int have_cmdline;
  int timeout;
} bios_config_t;

int bios_config_load(uint8_t drive, const void *unused, bios_config_t *out);

#define AURORA_BOOT_MAGIC 0xA0B00700u

/* ==========================================================================
 * API global
 * ========================================================================== */
void bios_log(const char *fmt, ...);
void bios_halt(void);

/* Memoria */
void zero_mem(void *dst, uint32_t n);
void copy_mem(void *dst, const void *src, uint32_t n);
int compare_mem(const void *a, const void *b, uint32_t n);
uint32_t str_len(const char *s);
int str_eq(const char *a, const char *b);
int str_eq_n(const char *a, const char *b, uint32_t n);
int str_eq_ci_n(const char *a, const char *b, uint32_t n);
void str_copy(char *dst, const char *src, uint32_t max);

/* int 13h */
int int13_read(uint8_t drive, uint32_t lba, uint16_t count, void *buf);
int int13_read_512(uint8_t drive, uint32_t lba, uint16_t count, void *buf);
int int13_reset(uint8_t drive);
int int13_get_params(uint8_t drive, uint16_t *sector_size_out,
                     uint32_t *total_sectors_out);

/* ISO9660 */
typedef struct {
  uint32_t lba;  /* LBA del fichero en el CD (sectores de 2048 B) */
  uint32_t size; /* tamano en bytes */
} iso_file_t;

int iso9660_mount(uint8_t drive);
int iso9660_find(const char *name_8_3_upper, iso_file_t *out);
int iso9660_read(uint8_t drive, const iso_file_t *f, uint32_t off, uint32_t len,
                 void *buf);

/* FAT16 sobre un rango de bloques del CD (efi.img incrustado) */
typedef struct {
  uint32_t first_cluster;
  uint32_t size;
} fat_file_t;

/* FAT16 sobre un rango de bloques: sustituye la antigua fat16_mount() */
int fat16_mount_iso(uint8_t drive, uint32_t iso_lba_2048,
                    uint32_t iso_size_bytes);
int fat16_mount_disk(uint8_t drive, uint32_t part_lba_512,
                     uint32_t part_size_bytes);
int fat16_find(const char *path_8_3, fat_file_t *out);
int fat16_read(uint8_t drive, const fat_file_t *f, uint32_t off, uint32_t len,
               void *buf);
uint32_t fat16_data_start_lba(void);
uint32_t fat16_cluster_size(void);

static inline int fat16_mount(uint8_t drive, uint32_t lba, uint32_t size) {
  return fat16_mount_iso(drive, lba, size);
}

/* ELF */
int elf_load(const uint8_t *elf_data, uint32_t elf_size, uint32_t *pml4,
             uint64_t *entry_out);

/* Paging */
uint32_t paging_build_identity_pml4(uint64_t max_phys);
int paging_map_4k(uint32_t *pml4, uint64_t virt, uint64_t phys);
int paging_map_2m(uint32_t *pml4, uint64_t virt, uint64_t phys);

/* E820 → EFI memmap */
uint32_t e820_to_efi(const e820_entry_t *e820, uint32_t count, uint8_t *out,
                     uint32_t out_size, const uint64_t *reserved_ranges,
                     uint32_t num_reserved);

/* ACPI */
void bios_find_rsdp(uint8_t out_rsdp[64]);

/* Bump allocator (paginas fisicas) */
void bump_init(const e820_entry_t *e820, uint32_t count);
uint32_t bump_alloc_page(void);
uint32_t bump_alloc_pages(uint32_t n);
uint32_t bump_alloc_low(uint32_t bytes, uint32_t align); /* < 1 MB */

/* Salto final */
void bios_jump_to_kernel(uint32_t pml4_phys, uint32_t boot_info_phys,
                         uint64_t kernel_entry);

/* Entry point desde stage2.asm */
void stage2_main(uint32_t drive, uint32_t e820_addr, uint32_t e820_count,
                 uint32_t vbe_desc_addr);

/* ---------------------------------------------------------------------
 * Acceso a puertos de I/O (x86-64/32).
 * NO uses *(volatile uint8_t *)0x3FD: eso lee MEMORIA, no el puerto.
 * --------------------------------------------------------------------- */
static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void) {
  /* Escribir a 0x80 es el "io delay" canónico. */
  __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

static inline void com1_putc(char c) {
  /* Esperar THRE (bit 5 del LSR) en el puerto 0x3FD. */
  while (!(inb(0x3FD) & 0x20)) {
    /* timeout simple no-op para no colgar si algo va mal */
  }
  outb(0x3F8, (uint8_t)c);
}

static inline void com1_puts(const char *s) {
  while (*s)
    com1_putc(*s++);
}

uint32_t bump_current(void);

#endif /* AURORA_BIOS_H */