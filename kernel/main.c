#include "ipc.h"
// kernel/main.c
// Punto de entrada del kernel Aurora OS

#include "../bootloader/include/kernel_boot_info.h"
#include "acpi.h"
#include "acpi_aml.h"
#include "acpi_pm.h"
#include "ahci.h"
#include "apic.h"
#include "ata_common.h"
#include "ata_dma.h"
#include "atapi.h"
#include "block.h"
#include "boot_console.h"
#include "cpu.h"
#include "driver.h"
#include "elf.h"
#include "fat32.h"
#include "gdt.h"
#include "gfx/compositor.h"
#include "gfx/theme.h"
#include "heap.h"
#include "idt.h"
#include "input.h"
#include "ipi.h"
#include "iso9660.h"
#include "klog.h"
#include "net/arp.h"
#include "net/dhcp.h"
#include "net/e1000e.h"
#include "net/icmp.h"
#include "net/ip.h"
#include "net/loopback.h"
#include "net/netif.h"
#include "net/ping.h"
#include "net/socket.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "paging.h"
#include "part.h"
#include "pci.h"
#include "pf.h"
#include "pmm.h"
#include "process.h"
#include "ps2.h"
#include "pty.h"
#include "rtc.h"
#include "sched.h"
#include "serial.h"
#include "simd.h"
#include "smp_boot.h"
#include "string.h"
#include "swap.h"
#include "syscall.h"
#include "tarfs.h"
#include "test.h"
#include "time.h"
#include "tty.h"
#include "vdso.h"
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Boot info (MISMO ORDEN Y TIPOS QUE EL BOOTLOADER)
// ---------------------------------------------------------------------------

static struct kernel_boot_info boot;
static struct kernel_boot_info g_boot_info;

uint32_t *fb_ptr = NULL;
uint32_t fb_width = 0;
uint32_t fb_height = 0;
uint32_t fb_pitch = 0;
uint32_t fb_bpp = 0;
static uint8_t *fb_hw_ptr = NULL;
static uint32_t fb_hw_pitch = 0;

static uint8_t syscall_kernel_stack[8192] __attribute__((aligned(16)));

extern struct driver ata_pio_driver;
extern struct driver atapi_driver;
extern struct driver ahci_driver;
extern struct driver xhci_driver;

extern uint8_t __text_start;
extern uint8_t __bss_end;

// ---------------------------------------------------------------------------
// Framebuffer helpers
// ---------------------------------------------------------------------------
static int fb_init(uint64_t base, uint32_t w, uint32_t h, uint32_t pitch,
                   uint32_t bpp, uint64_t size) {
  if (base == 0 || w == 0 || h == 0 || pitch == 0 || (bpp != 24 && bpp != 32) ||
      pitch < (uint64_t)w * (bpp / 8) || (uint64_t)pitch * h > size) {
    fb_ptr = NULL;
    fb_width = 0;
    fb_height = 0;
    fb_pitch = 0;
    fb_bpp = 0;
    fb_hw_ptr = NULL;
    fb_hw_pitch = 0;
    LOG_WARN(
        "[FB] parametros invalidos: base=%p %ux%u bpp=%u pitch=%u size=%lu",
        (void *)base, w, h, bpp, pitch, (unsigned long)size);
    return 0;
  }

  fb_hw_ptr = (uint8_t *)base;
  fb_hw_pitch = pitch;
  fb_width = w;
  fb_height = h;
  fb_bpp = bpp;
  fb_pitch = bpp == 24 ? w : pitch / 4;
  if (bpp == 24) {
    uint64_t pixels = (uint64_t)w * h;
    if (pixels > SIZE_MAX / sizeof(uint32_t)) {
      fb_ptr = NULL;
      fb_hw_ptr = NULL;
      LOG_ERR("[FB] dimensiones demasiado grandes para buffer RGB888");
      return 0;
    }
    fb_ptr = (uint32_t *)kmalloc((size_t)pixels * sizeof(uint32_t));
    if (!fb_ptr) {
      fb_hw_ptr = NULL;
      LOG_ERR("[FB] no se pudo reservar buffer RGB888 de %ux%u", w, h);
      return 0;
    }
  } else {
    fb_ptr = (uint32_t *)base;
  }
  LOG_INFO("[FB] Inicializado en memoria: %p %u x %u bpp=%u pitch=%u",
           (void *)base, w, h, bpp, pitch);
  return 1;
}

void fb_flush_rect(const uint32_t *src, uint32_t src_pitch, uint32_t x,
                   uint32_t y, uint32_t w, uint32_t h) {
  if (fb_bpp != 24 || !fb_hw_ptr || !src || x >= fb_width || y >= fb_height)
    return;
  if (w > fb_width - x)
    w = fb_width - x;
  if (h > fb_height - y)
    h = fb_height - y;
  for (uint32_t row = 0; row < h; row++) {
    uint8_t *dst =
        fb_hw_ptr + (uint64_t)(y + row) * fb_hw_pitch + (uint64_t)x * 3;
    const uint32_t *pixels = src + (uint64_t)(y + row) * src_pitch + x;
    for (uint32_t col = 0; col < w; col++) {
      uint32_t pixel = pixels[col];
      dst[col * 3 + 0] = (uint8_t)pixel;
      dst[col * 3 + 1] = (uint8_t)(pixel >> 8);
      dst[col * 3 + 2] = (uint8_t)(pixel >> 16);
    }
  }
}

static void fb_fill(uint32_t color) {
  if (!fb_ptr)
    return;
  for (uint32_t y = 0; y < fb_height; y++) {
    uint32_t *row = fb_ptr + (uint64_t)y * fb_pitch;
    for (uint32_t x = 0; x < fb_width; x++)
      row[x] = color;
  }
  fb_flush_rect(fb_ptr, fb_pitch, 0, 0, fb_width, fb_height);
}

// ---------------------------------------------------------------------------
// Memmap parse
// ---------------------------------------------------------------------------
#define EFI_CONVENTIONAL_MEMORY 7

static void parse_memmap(void) {
  if (!boot.memmap || boot.memmap_size == 0) {
    LOG_PANIC("[MEM] No hay mapa de memoria disponible");
    return;
  }
  LOG_INFO("[MEM] Mapa de memoria EFI:");
  uint8_t *ptr = (uint8_t *)boot.memmap;
  uint64_t total_usable = 0;

  for (uint64_t i = 0; i < boot.memmap_size; i += boot.memmap_desc_size) {
    uint32_t type = *(uint32_t *)(ptr + i + 0);
    uint64_t phys = *(uint64_t *)(ptr + i + 8);
    uint64_t pages = *(uint64_t *)(ptr + i + 24);
    uint64_t len = pages * 4096;

    if (type == EFI_CONVENTIONAL_MEMORY) {
      total_usable += len;
      LOG_INFO("  [USABLE] %p - %p (%lu MB)", (void *)phys,
               (void *)(phys + len), (unsigned long)(len / (1024 * 1024)));
    }
  }
  LOG_INFO("[MEM] Total usable: %lu MB",
           (unsigned long)(total_usable / (1024 * 1024)));
}

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------
static void sse_init(void) {
  uint64_t cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= 0x200;
  cr4 |= 0x400;
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));
  LOG_INFO("[SSE] OSFXSR + OSXMMEXCPT habilitados");
}

// ---------------------------------------------------------------------------
// Servicio IPC de eco
// ---------------------------------------------------------------------------
static uint32_t ipc_echo_task_id = 0;

static void ipc_echo_service(void) {
  task_t *self = sched_current();
  ipc_echo_task_id = self->id;
  syscall_register_service("echo", self->id);
  LOG_INFO("[IPC-KERNEL] Servicio de eco IPC iniciado (Task ID=%u)",
           ipc_echo_task_id);

  while (1) {
    ipc_msg_t msg;
    if (ipc_recv(&msg, 0) == 0) {
      LOG_INFO("[IPC-KERNEL] Mensaje recibido de Tarea ID=%u (tipo=%u, "
               "payload='%s')",
               msg.sender, msg.type, (const char *)msg.data);
      const char *reply = "PONG: Hola desde el Kernel (Ring 0)";
      ipc_send(msg.sender, IPC_TYPE_RESPONSE, reply, 36);
    }
  }
}

// ===========================================================================
// kmain_task
// ===========================================================================
static void kmain_task(void) {
  LOG_INFO("[KERNEL] kmain_task: inicio (id=%u)", sched_current()->id);

  sched_create_task(ipc_echo_service);

  extern void time_tick(void);
  irq_install_handler(0, time_tick);

  __asm__ volatile("sti");

  klog_calibrate_tsc(50, KERNEL_HZ);
  LOG_INFO("[TSC] Calibrado a %lu MHz", klog_get_tsc_freq() / 1000000);

  lapic_timer_init();

  klog_boot_step("[INIT] TTY...");
  tty_init();
  klog_boot_ok();

  klog_boot_step("[INIT] PTY pool...");
  pty_init();
  klog_boot_ok();

  klog_boot_step("[INIT] Block layer...");
  blk_init();
  klog_boot_ok();

  input_init();

  driver_register(&ps2_driver);
  driver_register(&pci_driver);
  driver_register(&ata_pio_driver);
  driver_register(&atapi_driver);
  driver_register(&ahci_driver);
  driver_register(&xhci_driver);
  drivers_init_all();

  extern int ata_pio_finalize(void);
  ata_pio_finalize();

  klog_boot_step("[INIT] Partition layer...");
  part_scan_all();
  klog_boot_ok();

  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || b->is_partition || b->is_read_only)
      continue;
    if (fat32_mount_bdev(b->name, "/data") == 0) {
      LOG_INFO("[INIT] Montado %s en /data", b->name);
      break;
    }
  }

  // Intentar montar CDs/DVDs (sector_size=2048, read-only) como ISO9660.
  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || b->is_partition)
      continue;
    if (b->sector_size != 2048)
      continue;
    if (iso9660_mount_bdev(b->name, "/cdrom") == 0) {
      LOG_INFO("[INIT] Montado %s en /cdrom (ISO9660)", b->name);
      break;
    }
  }
  LOG_INFO("[INIT] DMA dump...");
  ata_dma_dump();

  int fb_ok = (fb_ptr != NULL);
  if (fb_ok) {
    klog_boot_step("[FB] Framebuffer ya mapeado (WC)");
    klog_boot_ok();
  } else {
    LOG_ERR("[FB] sin framebuffer disponible");
  }

  LOG_INFO("[INIT] Aurora OS listo. Multitarea activa.");

  LOG_DEBUG("[SMP] Inicializando trampoline y APs...");
  smp_boot_init();
  smp_boot_aps();

  extern volatile int smp_sched_active;
  smp_sched_active = 1;

  smp_dump();
  acpi_dump();
  apic_dump();

  ipi_init();

  if (fb_ok) {
    // El compositor va a tomar el control. Desactivamos el hook normal
    // (ya no escribimos todo el log a pantalla) pero activamos el overlay
    // de las últimas líneas, para no perder visibilidad.
    boot_console_hook_remove();
    extern void boot_console_overlay_enable(int);
    boot_console_overlay_enable(1);
    compositor_init();
    LOG_INFO(
        "[KERNEL] Compositor inicializado. Cediendo control al scheduler...");
    sched_create_task(compositor_thread);
  }

  extern void __sched_canary_arm(void);
  __sched_canary_arm();

  {
    extern cpu_local_t cpu_local_data[];
    LOG_INFO("[DIAG-PRE-TEST] cpu_local_data[0].current_task=%p",
             (void *)cpu_local_data[0].current_task);
    LOG_INFO("[DIAG-PRE-TEST] cpu_local_data[0].cpu_id=%u",
             cpu_local_data[0].cpu_id);
  }

  int failed = run_all_tests();
  if (failed > 0) {
    LOG_ERR("[TEST] %d tests fallaron. Revisar arriba.", failed);
  }

  if (e1000e_netif()) {
    klog_boot_step("[INIT] Arrancando kthread RX de e1000e...");
    e1000e_start();
    klog_boot_ok();

    klog_boot_step("[INIT] Arrancando kthread DHCP...");
    dhcp_start(e1000e_netif());
    klog_boot_ok();

    klog_boot_step("[INIT] Arrancando kthread de ping (10.0.2.2)...");
    ping_init();
    klog_boot_ok();
  }

  __sched_canary_check();

  klog_boot_step("[INIT] Cargando init '/usr/bin/init'...");
  process_t *init_proc = process_load("/usr/bin/init");
  if (!init_proc) {
    klog_boot_warn("[INIT] /usr/bin/init no encontrado, fallback a "
                   "/usr/bin/terminal");
    process_load("/usr/bin/terminal");
  } else {
    klog_boot_ok();
  }

  return;
}

// ===========================================================================
// kmain
// ===========================================================================
// ===========================================================================
// kmain
// ===========================================================================
void kmain(struct kernel_boot_info *kinfo) {
  /* =====================================================================
   * [FIX-DEFENSIVO] Reinicializar IA32_GS_BASE a &cpu_local_data[0] aquí,
   * además de en boot.asm. Nunca está de más: si algo en el boot path
   * (BIOS o UEFI) sobrescribe el MSR, esto lo restaura antes de que
   * cualquier función lea %gs:0x10 (smp_processor_id / sched_current /
   * process_current / vfs_check_access / ...).
   *
   * NO depender de smp_init() para esto: se llama mucho más abajo, y
   * antes de llegar ahí ya hay varias rutas que consultan %gs.
   * ===================================================================== */
  {
    extern cpu_local_t cpu_local_data[];
    uint64_t gs_base = (uint64_t)&cpu_local_data[0];
    uint32_t lo = (uint32_t)(gs_base & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(gs_base >> 32);
    __asm__ volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(0xC0000101));
    __asm__ volatile("wrmsr" ::"a"(0), "d"(0), "c"(0xC0000102));
  }

  serial_init();
  klog_init();

  LOG_INFO("========================================");
  LOG_INFO("   AURORA OS KERNEL x86_64");
  LOG_INFO("   Fase 1 - Kernel Base (REV 4)");
  LOG_INFO("========================================");

  klog_boot_step("[INIT] SSE...");
  sse_init();
  klog_boot_ok();

  klog_boot_step("[INIT] SIMD...");
  cpu_simd_init();
  simd_init();
  klog_boot_ok();

  /* [PM] Detección temprana de capacidades de power management. */
  klog_boot_step("[INIT] PM: detección...");
  cpu_dump_cpuid_diag();
  cpu_detect_pm();
  klog_boot_ok();

  memcpy(&boot, kinfo, sizeof(boot));
  g_boot_info = boot;

  LOG_INFO("[BOOT] Framebuffer: %p %u x %u bpp=%u pitch=%u",
           (void *)boot.fb_base, boot.fb_width, boot.fb_height, boot.fb_bpp,
           boot.fb_pitch);

  klog_boot_step("[INIT] GDT...");
  gdt_init();
  klog_boot_ok();

  klog_boot_step("[INIT] IDT...");
  idt_init();
  klog_boot_ok();

  cpu_local_data[0].kernel_stack =
      (uint64_t)(syscall_kernel_stack + sizeof(syscall_kernel_stack));
  cpu_local_data[0].user_rsp = 0;

  klog_boot_step("[INIT] Syscalls...");
  syscall_init();
  klog_boot_ok();

  uint64_t max_phys_addr = 0;
  if (boot.memmap && boot.memmap_size > 0) {
    uint8_t *ptr = (uint8_t *)boot.memmap;
    for (uint64_t i = 0; i < boot.memmap_size; i += boot.memmap_desc_size) {
      uint32_t type = *(uint32_t *)(ptr + i + 0);
      if (type != EFI_CONVENTIONAL_MEMORY)
        continue;
      uint64_t phys = *(uint64_t *)(ptr + i + 8);
      uint64_t pages = *(uint64_t *)(ptr + i + 24);
      if (pages > UINT64_MAX / PAGE_SIZE) {
        LOG_WARN(
            "[MEM] Descriptor EFI con tamaño de páginas inválido, ignorado");
        continue;
      }
      uint64_t size = pages * PAGE_SIZE;
      if (phys > UINT64_MAX - size) {
        LOG_WARN("[MEM] Descriptor EFI con rango físico desbordado, ignorado");
        continue;
      }
      uint64_t end = phys + size;
      if (end > max_phys_addr)
        max_phys_addr = end;
    }
  }
  LOG_DEBUG("[INIT] max_phys_addr = %p (%lu MB)", (void *)max_phys_addr,
            (unsigned long)(max_phys_addr / (1024 * 1024)));

  if (boot.memmap == 0 || boot.memmap_size == 0 || boot.memmap_desc_size == 0) {
    LOG_WARN("[MEM] Memmap inválido o ausente. Abortando parse.");
  } else if (boot.memmap_size > (1024 * 1024)) {
    LOG_WARN("[MEM] Memmap demasiado grande, posible corrupción (size>%u)",
             1024 * 1024);
  } else {
    parse_memmap();
  }

  klog_boot_step("[INIT] PMM...");
  pmm_init(boot.memmap, boot.memmap_size, boot.memmap_desc_size);
  klog_boot_ok();

  extern void pmm_reserve_range(uint64_t start, uint64_t end);
  pmm_reserve_range(0x6000, 0x9000);

  extern void pmm_self_test(void);
  pmm_self_test();

  klog_boot_step("[INIT] Paging...");
  uint64_t cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  paging_init((uint64_t *)cr3, max_phys_addr);
  klog_boot_ok();

  /* [FIX] Reservar el rango físico REAL del kernel, página a página. */
  {
    uint64_t *pml4 = paging_get_pml4();
    uint64_t vstart = (uint64_t)&__text_start;
    uint64_t vend = (uint64_t)&__bss_end;

    uint64_t run_start = 0;
    uint64_t run_end = 0;
    int total_pages = 0;

    for (uint64_t v = vstart; v < vend; v += PAGE_SIZE) {
      uint64_t p = paging_get_phys_in(pml4, v);
      if (!p) {
        if (run_start) {
          pmm_reserve_range(run_start, run_end);
          run_start = 0;
        }
        continue;
      }
      if (run_start && p == run_end) {
        run_end += PAGE_SIZE;
      } else {
        if (run_start)
          pmm_reserve_range(run_start, run_end);
        run_start = p;
        run_end = p + PAGE_SIZE;
      }
      total_pages++;
    }
    if (run_start)
      pmm_reserve_range(run_start, run_end);

    LOG_INFO("[INIT] Kernel phys reservado: %d páginas (%lu KB)", total_pages,
             (unsigned long)((uint64_t)total_pages * PAGE_SIZE / 1024));
  }

  pmm_relocate_bitmap();
  uaccess_init();

  klog_boot_step("[INIT] Heap (kmalloc)...");
  heap_init();
  klog_boot_ok();

  /* [FB] Init temprano con mmio_map. */
  if (boot.fb_base && boot.fb_width > 0 && boot.fb_height > 0 &&
      boot.fb_pitch > 0 && boot.fb_size > 0) {
    void *fb_virt = mmio_map(boot.fb_base, boot.fb_size,
                             PTE_WRITABLE | PTE_WRITECOMB | PTE_NX);
    if (fb_virt && fb_init((uint64_t)fb_virt, boot.fb_width, boot.fb_height,
                           boot.fb_pitch, boot.fb_bpp, boot.fb_size)) {
      fb_fill(0x00101828);
      boot_console_init(fb_ptr, fb_width, fb_height, fb_pitch);
      boot_console_clear();
      boot_console_hook_install();
      LOG_INFO("[FB] Boot console activa (%ux%u, phys=0x%lx)", fb_width,
               fb_height, (unsigned long)boot.fb_base);
    } else if (!fb_virt) {
      LOG_WARN("[FB] mmio_map del framebuffer falló");
    }
  } else if (boot.fb_base) {
    LOG_WARN("[FB] parámetros inválidos: base=0x%lx w=%u h=%u pitch=%u",
             (unsigned long)boot.fb_base, boot.fb_width, boot.fb_height,
             boot.fb_pitch);
  }

  klog_boot_step("[INIT] ACPI (MADT)...");
  acpi_init(boot.acpi_rsdp);
  klog_boot_ok();

  klog_boot_step("[INIT] ACPI AML (namespace)...");
  aml_init();
  klog_boot_ok();

  klog_boot_step("[INIT] ACPI PM (P-states, S-states)...");
  acpi_pm_init();
  klog_boot_ok();

  klog_boot_step("[INIT] PM: aplicar política...");
  cpu_apply_pm_policy();
  klog_boot_ok();

  klog_boot_step("[INIT] APIC (LAPIC + IOAPIC)...");
  apic_init();
  klog_boot_ok();

  klog_boot_step("[INIT] Demand paging (PF handler)...");
  pf_init();
  klog_boot_ok();

  klog_boot_step("[INIT] Swap subsystem...");
  swap_init();
  klog_boot_ok();

  pit_init();

  klog_boot_step("[INIT] Initramfs (TarFS)...");
  if (boot.initrd_base == 0 || boot.initrd_size == 0) {
    klog_boot_warn("[INIT] initrd no cargado por el bootloader");
  } else {
    const void *initrd_virt = phys_to_virt(boot.initrd_base);
    size_t initrd_size = (size_t)boot.initrd_size;
    LOG_INFO("[INIT] initrd en phys=0x%lx (%lu KB) -> virt=%p",
             (unsigned long)boot.initrd_base,
             (unsigned long)(initrd_size / 1024), initrd_virt);
    tarfs_init(initrd_virt, initrd_size);
  }
  vfs_init();
  ipc_init();
  klog_boot_ok();

  klog_boot_step("[INIT] RTC CMOS...");
  rtc_init();
  klog_boot_ok();

  klog_boot_step("[INIT] vDSO...");
  vdso_init();
  klog_boot_ok();

  klog_boot_step("[INIT] Networking (Fase 0)...");
  net_init();
  klog_boot_ok();

  klog_boot_step("[INIT] Loopback...");
  loopback_init();
  klog_boot_ok();

  klog_boot_step("[INIT] IPv4...");
  ip_init();
  klog_boot_ok();

  klog_boot_step("[INIT] ARP...");
  arp_init();
  klog_boot_ok();

  klog_boot_step("[INIT] ICMP...");
  icmp_init();
  klog_boot_ok();

  klog_boot_step("[INIT] UDP...");
  udp_init();
  klog_boot_ok();

  klog_boot_step("[INIT] Sockets...");
  socket_subsystem_init();
  klog_boot_ok();

  klog_boot_step("[INIT] TCP...");
  tcp_init();
  klog_boot_ok();

  klog_boot_step("[INIT] e1000e...");
  if (e1000e_init() == 0) {
    klog_boot_ok();
  } else {
    klog_boot_warn("no disponible");
  }

  klog_boot_step("[INIT] Scheduler...");
  smp_init();
  smp_set_bsp_lapic_id(lapic_get_bsp_id());
  sched_init();
  klog_boot_ok();

  klog_boot_step("[INIT] Process subsystem...");
  extern void process_init(void);
  process_init();
  klog_boot_ok();

  klog_boot_step("[INIT] kswapd...");
  extern void kswapd_main(void);
  task_t *kswapd_t = sched_create_task(kswapd_main);
  if (kswapd_t) {
    klog_boot_ok();
    LOG_INFO("[INIT] kswapd lanzado (task ID=%u)", kswapd_t->id);
  } else {
    klog_boot_warn("[INIT] kswapd no se pudo lanzar (sin memoria)");
  }

  task_t *t = sched_create_task(kmain_task);
  if (!t) {
    LOG_PANIC("[KERNEL] no se pudo crear kmain_task");
  }
  t->cpu_affinity = 0;

  LOG_DEBUG("[KERNEL] kmain: saltando a kmain_task");
  sched_start(t);

  __builtin_unreachable();
}