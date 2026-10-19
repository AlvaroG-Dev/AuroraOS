/* bootloader/bios/bios_main.c */
#include "bios.h"
#include "fat16.h"
#include "splash.h"

/* ===========================================================================
 * Salida SOLO por pantalla VGA (0xB8000).
 * Desactivamos com1_putc porque la UART real del portátil se bloquea
 * cuando su FIFO de 64 bytes se llena (control de flujo por hardware).
 * ===========================================================================
 */
static int g_vga_row = 0;
static int g_vga_col = 0;

static void vga_scroll(void) {
  uint16_t *vga = (uint16_t *)0xB8000;
  for (int i = 0; i < 24 * 80; i++)
    vga[i] = vga[i + 80];
  for (int i = 24 * 80; i < 25 * 80; i++)
    vga[i] = 0x0F00 | ' ';
  g_vga_row = 24;
  g_vga_col = 0;
}

static void vga_putc(char c) {
  uint16_t *vga = (uint16_t *)0xB8000;
  if (c == '\n') {
    g_vga_col = 0;
    g_vga_row++;
  } else if (c == '\r') {
    g_vga_col = 0;
  } else {
    vga[g_vga_row * 80 + g_vga_col] = 0x0F00 | (uint8_t)c;
    g_vga_col++;
    if (g_vga_col >= 80) {
      g_vga_col = 0;
      g_vga_row++;
    }
  }
  if (g_vga_row >= 25)
    vga_scroll();
}

static void log_char(char c) {
  vga_putc(c);
  outb(0x3F8, (uint8_t)c);
}

static void out_u32_port(uint32_t v, uint32_t base) {
  char buf[16];
  int n = 0;
  if (v == 0)
    buf[n++] = '0';
  while (v) {
    uint32_t d = v % base;
    buf[n++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
    v /= base;
  }
  while (n > 0)
    log_char(buf[--n]);
}

static void out_str_port(const char *s) {
  while (*s)
    log_char(*s++);
}

void bios_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  while (*fmt) {
    if (*fmt != '%') {
      log_char(*fmt++);
      continue;
    }
    fmt++;
    switch (*fmt) {
    case 'd': {
      int32_t v = va_arg(ap, int32_t);
      if (v < 0) {
        log_char('-');
        v = -v;
      }
      out_u32_port((uint32_t)v, 10);
      break;
    }
    case 'u':
      out_u32_port(va_arg(ap, uint32_t), 10);
      break;
    case 'x':
      out_u32_port(va_arg(ap, uint32_t), 16);
      break;
    case 'p':
      out_str_port("0x");
      out_u32_port(va_arg(ap, uint32_t), 16);
      break;
    case 's':
      out_str_port(va_arg(ap, const char *));
      break;
    case 'c':
      log_char((char)va_arg(ap, int));
      break;
    case 'l': {
      fmt++;
      if (*fmt == 'x')
        out_u32_port(va_arg(ap, uint32_t), 16);
      else if (*fmt == 'u')
        out_u32_port(va_arg(ap, uint32_t), 10);
      break;
    }
    case '%':
      log_char('%');
      break;
    default:
      log_char('?');
      break;
    }
    fmt++;
  }
  va_end(ap);
}

void bios_halt(void) {
  while (1)
    __asm__ volatile("cli; hlt");
}

/* ===========================================================================
 * ACPI RSDP
 * ===========================================================================
 */
static int rsdp_valid(const uint8_t *p) {
  if (!str_eq_n((const char *)p, "RSD PTR ", 8))
    return 0;
  uint8_t sum = 0;
  for (int i = 0; i < 20; i++)
    sum += p[i];
  return sum == 0;
}

void bios_find_rsdp(uint8_t out_rsdp[64]) {
  zero_mem(out_rsdp, 64);
  uint16_t ebda_seg = *(uint16_t *)0x40E;
  uint32_t ebda = (uint32_t)ebda_seg << 4;
  if (ebda >= 0x400 && ebda < 0xA0000) {
    for (uint32_t off = 0; off < 1024; off += 16) {
      const uint8_t *p = (const uint8_t *)(ebda + off);
      if (rsdp_valid(p)) {
        copy_mem(out_rsdp, p, 64);
        return;
      }
    }
  }
  for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
    const uint8_t *p = (const uint8_t *)a;
    if (rsdp_valid(p)) {
      copy_mem(out_rsdp, p, 64);
      return;
    }
  }
}

extern void vbe_get(vbe_desc_t *out);
extern void rm_vbe_set_mode(uint16_t mode);

/* ===========================================================================
 * stage2_main
 * ===========================================================================
 */
void stage2_main(uint32_t drive, uint32_t e820_addr, uint32_t e820_count,
                 uint32_t vbe_desc_addr) {
  (void)vbe_desc_addr;

  /* Limpiar pantalla VGA al entrar */
  uint16_t *vga = (uint16_t *)0xB8000;
  for (int i = 0; i < 25 * 80; i++)
    vga[i] = 0x0F00 | ' ';
  g_vga_row = 0;
  g_vga_col = 0;

  bios_log("\n=== AURORA BIOS BOOTLOADER ===\n");
  bios_log("[BIOS] drive=%u e820_count=%u\n", drive, e820_count);

  const e820_entry_t *e820 = (const e820_entry_t *)e820_addr;

  bump_init(e820, e820_count);

  /* Detectar medio: CD o USB */
  int is_cd = 0;
  /* [FIX] Si drive es 0x80, es un disco/USB. No intentamos leer LBA 16
   * porque el int 13h AH=42h en hardware real con USB puede colgar. */
  if (drive != 0x80) {
    static uint8_t probe[2048] __attribute__((aligned(16)));
    if (int13_read(drive, 16, 1, probe) == 0) {
      if (probe[0] == 1 && str_eq_n((char *)(probe + 1), "CD001", 5))
        is_cd = 1;
    }
  }

  if (is_cd) {
    bios_log("[BOOT] medio: CD (ISO9660)\n");
    if (iso9660_mount(drive) != 0) {
      bios_log("[ISO] mount fallo\n");
      bios_halt();
    }
    iso_file_t efi_img;
    if (iso9660_find("efi.img", &efi_img) != 0) {
      bios_log("[ISO] /EFI.IMG no encontrado\n");
      bios_halt();
    }
    bios_log("[ISO] /EFI.IMG en LBA %u, %u bytes\n", efi_img.lba, efi_img.size);
    if (fat16_mount_iso(drive, efi_img.lba, efi_img.size) != 0) {
      bios_log("[FAT] mount fallo\n");
      bios_halt();
    }
  } else {
    bios_log("[BOOT] medio: disco/USB (MBR)\n");
    uint8_t mbr[512];
    /* [FIX] Leer el MBR de 0x7C00 directamente. Evitamos int 13h aquí
     * porque muchas BIOS de portátiles fallan si el buffer está en la pila. */
    copy_mem(mbr, (void *)0x7C00, 512);

    uint32_t part_lba = 0, part_sectors = 0;
    for (int i = 0; i < 4; i++) {
      uint8_t *p = mbr + 446 + i * 16;
      /* Aceptamos 0xEF (EFI System Partition) o 0x0C (FAT32 LBA) */
      if (p[4] == 0xEF || p[4] == 0x0C) {
        part_lba = *(uint32_t *)(p + 8);
        part_sectors = *(uint32_t *)(p + 12);
        break;
      }
    }
    if (part_lba == 0) {
      bios_log("[MBR] particion EF no encontrada\n");
      bios_halt();
    }
    bios_log("[MBR] particion EF: LBA %u, %u sectores\n", part_lba,
             part_sectors);
    if (fat16_mount_disk(drive, part_lba, part_sectors * 512) != 0) {
      bios_log("[FAT] mount fallo\n");
      bios_halt();
    }
  }
  bios_log("[FAT] mount OK\n");

  /* Fijar VBE temprano: sin esto, el LFB no está disponible hasta
   * el final del boot. rm_vbe_set_mode preserva el estado y se puede
   * llamar varias veces sin problema. */
  vbe_desc_t vbe_early;
  vbe_get(&vbe_early);
  int splash_on = 0;
  if (vbe_early.ok && vbe_early.lfb_phys) {
    rm_vbe_set_mode(vbe_early.mode);
    splash_on =
        (bios_splash_init(vbe_early.lfb_phys, vbe_early.width, vbe_early.height,
                          vbe_early.pitch, vbe_early.bpp) == 0);
    if (splash_on)
      bios_log("[SPLASH] activa %ux%u bpp=%u\n", vbe_early.width,
               vbe_early.height, vbe_early.bpp);
  }

  bios_config_t cfg;
  bios_config_load(drive, NULL, &cfg);

  fat_file_t kernel_file;
  int kernel_found = 0;
  if (cfg.have_kernel && fat16_find(cfg.kernel_path, &kernel_file) == 0) {
    kernel_found = 1;
    bios_log("[FAT] kernel desde conf: %s\n", cfg.kernel_path);
  }
  if (!kernel_found) {
    static const char *cands[] = {"/AURORA/KERNEL.ELF", "/KERNEL.ELF"};
    for (int i = 0; i < 2 && !kernel_found; i++) {
      if (fat16_find(cands[i], &kernel_file) == 0) {
        kernel_found = 1;
        bios_log("[FAT] kernel por candidato: %s\n", cands[i]);
      }
    }
  }
  if (!kernel_found) {
    bios_log("[FAT] kernel.elf no encontrado\n");
    bios_halt();
  }
  bios_log("[FAT] kernel: %u bytes (cluster %u)\n", kernel_file.size,
           kernel_file.first_cluster);

  if (kernel_file.size > KERNEL_STAGE_MAX) {
    bios_log("[FAT] kernel demasiado grande\n");
    bios_halt();
  }
  if (fat16_read(drive, &kernel_file, 0, kernel_file.size,
                 (void *)KERNEL_STAGE_ADDR) != (int)kernel_file.size) {
    bios_log("[FAT] read kernel fallo\n");
    bios_halt();
  }
  bios_log("[FAT] kernel cargado en 0x%x\n", KERNEL_STAGE_ADDR);

  fat_file_t initrd_file;
  uint64_t initrd_phys = 0, initrd_size = 0;
  int initrd_found = 0;
  if (cfg.have_initrd && fat16_find(cfg.initrd_path, &initrd_file) == 0) {
    initrd_found = 1;
    bios_log("[FAT] initrd desde conf: %s\n", cfg.initrd_path);
  }
  if (!initrd_found) {
    static const char *cands[] = {"/AURORA/INITRD.TAR", "/INITRD.TAR"};
    for (int i = 0; i < 2 && !initrd_found; i++) {
      if (fat16_find(cands[i], &initrd_file) == 0) {
        initrd_found = 1;
        bios_log("[FAT] initrd por candidato: %s\n", cands[i]);
      }
    }
  }

  if (initrd_found) {
    bios_log("[FAT] initrd: %u bytes\n", initrd_file.size);
    initrd_size = initrd_file.size;
    uint32_t npages = (initrd_file.size + 0xFFF) / 0x1000;
    uint32_t phys = bump_alloc_pages(npages);
    if (!phys) {
      bios_log("[FAT] initrd alloc fallo\n");
      initrd_phys = 0;
      initrd_size = 0;
    } else {
      initrd_phys = phys;
      bios_log("[FAT] initrd @ 0x%x (%u pages)\n", phys, npages);

      uint32_t data_start_512 = fat16_data_start_lba();
      uint32_t cluster_size = fat16_cluster_size();
      uint32_t byte_off_in_fs =
          data_start_512 * 512u +
          (uint32_t)(initrd_file.first_cluster - 2) * cluster_size;

      uint32_t remaining = initrd_file.size;
      uint32_t dst = phys;
      int read_ok = 1;
      uint32_t it = 0;

      if (is_cd) {
        iso_file_t efi_img2;
        iso9660_find("efi.img", &efi_img2);
        uint32_t lba = efi_img2.lba + (byte_off_in_fs / 2048u);
        while (remaining > 0) {
          uint32_t nsec = (remaining + 2047u) / 2048u;
          if (nsec > 32u)
            nsec = 32u;
          uint32_t nbytes = nsec * 2048u;
          if (nbytes > remaining)
            nbytes = remaining;
          if (int13_read(drive, lba, (uint16_t)nsec, (void *)dst) != 0) {
            bios_log("[READ] FAIL en lba=%u\n", lba);
            read_ok = 0;
            break;
          }
          dst += nbytes;
          lba += nsec;
          remaining -= nbytes;
          bios_splash_tick();
          if (++it % 100 == 0) {
            bios_log("[READ] %u / %u KB\n", (dst - phys) / 1024,
                     initrd_file.size / 1024);
          }
        }
      } else {
        uint32_t part_lba = 0;
        {
          uint8_t mbr[512];
          int13_read_512(drive, 0, 1, mbr);
          for (int i = 0; i < 4; i++) {
            uint8_t *p = mbr + 446 + i * 16;
            if (p[4] == 0xEF || p[4] == 0x0C) {
              part_lba = *(uint32_t *)(p + 8);
              break;
            }
          }
        }
        uint32_t lba = part_lba + (byte_off_in_fs / 512u);
        while (remaining > 0) {
          uint32_t nsec = (remaining + 511u) / 512u;
          if (nsec > 64u)
            nsec = 64u;
          uint32_t nbytes = nsec * 512u;
          if (nbytes > remaining)
            nbytes = remaining;
          if (int13_read_512(drive, lba, (uint16_t)nsec, (void *)dst) != 0) {
            bios_log("[READ] FAIL en lba=%u\n", lba);
            read_ok = 0;
            break;
          }
          dst += nbytes;
          lba += nsec;
          remaining -= nbytes;
          bios_splash_tick();
          if (++it % 200 == 0) {
            bios_log("[READ] %u / %u KB\n", (dst - phys) / 1024,
                     initrd_file.size / 1024);
          }
        }
      }
      bios_log("[READ] fin: ok=%d leido=%u\n", read_ok, (uint32_t)(dst - phys));
      if (!read_ok) {
        initrd_phys = 0;
        initrd_size = 0;
      }
    }
  } else {
    bios_log("[FAT] initrd.tar no encontrado\n");
  }

  uint32_t pml4_phys = paging_build_identity_pml4(0x100000000ULL);
  if (!pml4_phys) {
    bios_log("[PG] fallo\n");
    bios_halt();
  }
  bios_log("[PG] PML4 en 0x%x\n", pml4_phys);

  uint64_t entry = 0;
  if (elf_load((const uint8_t *)KERNEL_STAGE_ADDR, kernel_file.size,
               (uint32_t *)pml4_phys, &entry) != 0) {
    bios_log("[ELF] load fallo\n");
    bios_halt();
  }
  bios_log("[ELF] entry=0x%lx\n", (unsigned long)entry);

  vbe_desc_t vbe;
  vbe_get(&vbe);
  bios_log("[VBE] ok=%u %ux%u bpp=%u pitch=%u lfb=0x%x\n", vbe.ok, vbe.width,
           vbe.height, vbe.bpp, vbe.pitch, vbe.lfb_phys);

  uint8_t rsdp[64];
  bios_find_rsdp(rsdp);

  static uint8_t emap[4096];
  uint64_t reserved[32];
  uint32_t nres = 0;
  reserved[nres++] = 0x00000000;
  reserved[nres++] = 0x00100000;
  uint32_t bump_end = bump_current();
  if (bump_end > KERNEL_STAGE_ADDR) {
    reserved[nres++] = KERNEL_STAGE_ADDR;
    reserved[nres++] = bump_end;
  }
  uint32_t emap_size =
      e820_to_efi(e820, e820_count, emap, sizeof(emap), reserved, nres);
  bios_log("[MEM] memmap: %u bytes\n", emap_size);
  uint8_t *emap_final = (uint8_t *)0x30000;
  copy_mem(emap_final, emap, emap_size);

  static struct kernel_boot_info kinfo;
  zero_mem(&kinfo, sizeof(kinfo));
  if (vbe.ok) {
    kinfo.fb_base = vbe.lfb_phys;
    kinfo.fb_size = (uint64_t)vbe.pitch * vbe.height;
    kinfo.fb_width = vbe.width;
    kinfo.fb_height = vbe.height;
    kinfo.fb_pitch = vbe.pitch;
    kinfo.fb_bpp = vbe.bpp;
  }
  kinfo.memmap = (uint64_t)(uintptr_t)emap_final;
  kinfo.memmap_size = emap_size;
  kinfo.memmap_desc_size = sizeof(efi_memory_descriptor_t);
  kinfo.memmap_desc_ver = 1;
  copy_mem(kinfo.acpi_rsdp, rsdp, 64);
  kinfo.initrd_base = initrd_phys;
  kinfo.initrd_size = initrd_size;

  struct kernel_boot_info *kinfo_final =
      (struct kernel_boot_info *)(BOOT_PARAMS_ADDR + 0x100);
  copy_mem(kinfo_final, &kinfo, sizeof(kinfo));

  bios_log("[BOOT] saltando al kernel en 0x%lx\n", (unsigned long)entry);

  if (splash_on)
    bios_splash_done();

  /* Si no se fijó el modo antes (vbe.ok era 0 en su momento), fijarlo
   * ahora por si acaso. Doble llamada es segura. */
  if (vbe.ok && !splash_on)
    rm_vbe_set_mode(vbe.mode);

  bios_jump_to_kernel(pml4_phys, (uint32_t)(uintptr_t)kinfo_final, entry);
  bios_halt();
}