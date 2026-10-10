#include "ipc.h"
// kernel/main.c
// Punto de entrada del kernel Aurora OS

#include "acpi.h"
#include "ahci.h"
#include "apic.h"
#include "ata_common.h"
#include "ata_dma.h"
#include "atapi.h"
#include "block.h"
#include "cpu.h"
#include "driver.h"
#include "elf.h" // [FIX] Elf64_Ehdr, Elf64_Phdr, PT_LOAD
#include "fat32.h"
#include "gdt.h"
#include "gfx/compositor.h"
#include "gfx/theme.h"
#include "heap.h"
#include "idt.h"
#include "initrd.h"
#include "input.h"
#include "ipi.h"
#include "klog.h"
#include "net/arp.h"
#include "net/e1000e.h"
#include "net/icmp.h"
#include "net/ip.h"
#include "net/loopback.h"
#include "net/netif.h"
#include "net/ping.h"
#include "net/socket.h"
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
#include "swap.h" // [SWAP]
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

static struct kernel_boot_info boot;
static struct kernel_boot_info g_boot_info;

uint32_t *fb_ptr = NULL;
uint32_t fb_width = 0;
uint32_t fb_height = 0;
uint32_t fb_pitch = 0;

static uint8_t syscall_kernel_stack[8192] __attribute__((aligned(16)));

extern struct driver ata_pio_driver;
extern struct driver atapi_driver;
extern struct driver ahci_driver;

// [FIX] Símbolos del linker para el rango físico REAL del kernel.
// NO usamos `_kernel_end - KERNEL_VMA`: esa es la LMA (load memory address)
// que sugiere el linker script, no la dirección física donde el bootloader
// cargó el kernel. Cuando el kernel crece (libc.a +9 MB), el bootloader
// coloca los segmentos en direcciones físicas distintas a la LMA, y una
// reserva basada en LMA deja páginas del kernel sin reservar. El PMM las
// entrega al heap, el heap las pisa, y todo explota. Usamos paging_get_phys_in
// para obtener la dirección física real via la tabla de páginas ya montada.
extern uint8_t __text_start;
extern uint8_t __bss_end;

// ---------------------------------------------------------------------------
// Framebuffer helpers
// ---------------------------------------------------------------------------
static void fb_init(uint64_t base, uint32_t w, uint32_t h, uint32_t pitch) {
  if (base == 0 || w == 0 || h == 0 || pitch == 0) {
    fb_ptr = NULL;
    fb_width = 0;
    fb_height = 0;
    fb_pitch = 0;
    return;
  }
  fb_ptr = (uint32_t *)base;
  fb_width = w;
  fb_height = h;
  fb_pitch = pitch / 4;
  LOG_INFO("[FB] Inicializado en memoria: %p %u x %u pitch=%u", (void *)base, w,
           h, pitch);
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

  LOG_INFO("[INIT] TTY...");
  tty_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] PTY pool...");
  pty_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Block layer...");
  blk_init();
  LOG_INFO("OK");

  input_init();

  driver_register(&ps2_driver);
  driver_register(&pci_driver);
  driver_register(&ata_pio_driver);
  driver_register(&atapi_driver);
  driver_register(&ahci_driver);
  drivers_init_all();

  extern int ata_pio_finalize(void);
  ata_pio_finalize();

  LOG_INFO("[INIT] Partition layer...");
  part_scan_all();
  LOG_INFO("OK");

  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || b->is_partition || b->is_read_only)
      continue;
    if (fat32_mount_bdev(b->name, "/data") == 0) {
      LOG_INFO("[INIT] Montado %s en /data", b->name);
      break;
    }
  }

  // /tmp ahora es tmpfs (montado en vfs_init). No hay bind a /data/tmp.

  LOG_INFO("[INIT] DMA dump...");
  ata_dma_dump();

  int fb_ok = 0;
  if (g_boot_info.fb_base != 0 && g_boot_info.fb_width > 0 &&
      g_boot_info.fb_height > 0) {
    LOG_INFO(
        "[FB] Mapeando framebuffer via MMIO_MAP_BASE (Write-Combining)... ");
    void *fb_virt = mmio_map(g_boot_info.fb_base, g_boot_info.fb_size,
                             PTE_WRITABLE | PTE_WRITECOMB | PTE_NX);
    if (fb_virt) {
      LOG_INFO("OK");
      fb_ok = 1;
      fb_init((uint64_t)fb_virt, g_boot_info.fb_width, g_boot_info.fb_height,
              g_boot_info.fb_pitch);
    } else {
      LOG_ERR("FALLIDO (mmio_map devolvió NULL)");
    }
  }
  if (!fb_ok) {
    LOG_ERR("[FB] No hay framebuffer disponible o fallo al mapear");
  }

  LOG_INFO("[INIT] Aurora OS listo. Multitarea activa.");
  if (fb_ok) {
    compositor_init();
    LOG_INFO(
        "[KERNEL] Compositor inicializado. Cediendo control al scheduler...");
    sched_create_task(compositor_thread);
  }

  LOG_DEBUG("[SMP] Inicializando trampoline y APs...");
  smp_boot_init();
  smp_boot_aps();

  extern volatile int smp_sched_active;
  smp_sched_active = 1;

  smp_dump();
  acpi_dump();
  apic_dump();

  ipi_init();

  extern void __sched_canary_arm(void);
  __sched_canary_arm();

  int failed = run_all_tests();
  if (failed > 0) {
    LOG_ERR("[TEST] %d tests fallaron. Revisar arriba.", failed);
  }

  // Arranca el kthread de RX del driver e1000e (si se inicializó).
  if (e1000e_netif()) {
    LOG_INFO("[INIT] Arrancando kthread RX de e1000e...");
    e1000e_start();

    LOG_INFO("[INIT] Arrancando kthread de ping (10.0.2.2)...");
    ping_init();
  }

  __sched_canary_check();

  LOG_INFO("[INIT] Cargando init '/usr/bin/init'...");
  process_t *init_proc = process_load("/usr/bin/init");
  if (!init_proc) {
    LOG_WARN(
        "[INIT] /usr/bin/init no encontrado, fallback a /usr/bin/terminal");
    process_load("/usr/bin/terminal");
  }

  return;
}

// ===========================================================================
// kmain
// ===========================================================================
void kmain(struct kernel_boot_info *kinfo) {
  serial_init();
  klog_init();
  LOG_INFO("========================================");
  LOG_INFO("   AURORA OS KERNEL x86_64");
  LOG_INFO("   Fase 1 - Kernel Base (REV 4)");
  LOG_INFO("========================================");

  LOG_INFO("[INIT] SSE... ");
  sse_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] SIMD... ");
  cpu_simd_init();
  simd_init();
  LOG_INFO("OK");

  memcpy(&boot, kinfo, sizeof(boot));
  g_boot_info = boot;

  LOG_INFO("[BOOT] Framebuffer: %p %u x %u pitch=%u", (void *)boot.fb_base,
           boot.fb_width, boot.fb_height, boot.fb_pitch);

  LOG_INFO("[INIT] GDT... ");
  gdt_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] IDT... ");
  idt_init();
  LOG_INFO("OK");

  cpu_local_data[0].kernel_stack =
      (uint64_t)(syscall_kernel_stack + sizeof(syscall_kernel_stack));
  cpu_local_data[0].user_rsp = 0;

  LOG_INFO("[INIT] Syscalls... ");
  syscall_init();
  LOG_INFO("OK");

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
    LOG_INFO("[MEM] Memmap inválido o ausente. Abortando parse.");
  } else if (boot.memmap_size > (1024 * 1024)) {
    LOG_WARN("[MEM] Memmap demasiado grande, posible corrupción (size>%u)",
             1024 * 1024);
  } else {
    parse_memmap();
  }

  LOG_INFO("[INIT] Iniciando PMM...");
  pmm_init(boot.memmap, boot.memmap_size, boot.memmap_desc_size);

  // [SMP] Reservar las páginas bajas del trampoline ANTES del auto-test.
  // Rango: 0x6000..0x9000 (stack 16-bit + código + smp_boot_params).
  extern void pmm_reserve_range(uint64_t start, uint64_t end);
  pmm_reserve_range(0x6000, 0x9000);

  extern void pmm_self_test(void);
  pmm_self_test();

  LOG_INFO("[INIT] Paging... ");
  uint64_t cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  paging_init((uint64_t *)cr3, max_phys_addr);
  LOG_INFO("OK");

  // ---------------------------------------------------------------------------
  // [FIX] Reservar el rango físico REAL del kernel, página a página.
  //
  // El kernel tiene 2 PT_LOAD y el bootloader los coloca en zonas físicas
  // disjuntas (no contiguas): .text/.rodata/.data/initrd en una zona alta,
  // y .bss en otra zona baja. NO podemos usar un único rango
  // [phys(__text_start), phys(__bss_end)) porque los extremos no están
  // ordenados.
  //
  // Tampoco podemos leer el ELF header desde memoria: el bootloader solo
  // carga los PT_LOAD, no el header del fichero. Cualquier intento de
  // leer e_phoff desde 0xFFFFFFFF81000000 lee bytes de .text, no del header.
  //
  // Solución: recorrer cada página virtual de [__text_start, __bss_end)
  // y reservar su dirección física individualmente. Las páginas no mapeadas
  // (huecos entre segmentos) se saltan solas.
  // ---------------------------------------------------------------------------
  {
    uint64_t *pml4 = paging_get_pml4();
    uint64_t vstart = (uint64_t)&__text_start;
    uint64_t vend = (uint64_t)&__bss_end;

    // Optimización: agrupar páginas físicas contiguas en un solo
    // pmm_reserve_range. Aunque los PT_LOAD están disjuntos en físico,
    // dentro de cada segmento son contiguos, así que esto reduce las
    // llamadas de ~35000 a 2.
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

  // [FIX SMAP] Parchear stac/clac a NOP si la CPU no soporta SMAP.
  // Debe ejecutarse ANTES de cualquier código que use uaccess.
  uaccess_init();

  LOG_INFO("[INIT] ACPI (MADT)...");
  acpi_init(boot.acpi_rsdp);
  LOG_INFO("OK");

  LOG_INFO("[INIT] APIC (LAPIC + IOAPIC)...");
  apic_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Demand paging (PF handler)...");
  pf_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Iniciando Heap (kmalloc)...");
  heap_init();

  LOG_INFO("[INIT] Swap subsystem...");
  swap_init();
  LOG_INFO("OK");

  pit_init();

  LOG_INFO("[INIT] Inicializando Initramfs (TarFS)...");
  size_t initrd_size = (size_t)(initrd_end - initrd_start);
  tarfs_init(initrd_start, initrd_size);
  vfs_init();
  ipc_init();

  LOG_INFO("[INIT] RTC CMOS... ");
  rtc_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] vDSO... ");
  vdso_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Networking (Fase 0)... ");
  net_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Loopback... ");
  loopback_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] IPv4... ");
  ip_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] ARP... ");
  arp_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] ICMP... ");
  icmp_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] UDP... ");
  udp_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Sockets... ");
  socket_subsystem_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] e1000e... ");
  if (e1000e_init() == 0) {
    LOG_INFO("OK");
  } else {
    LOG_WARN("no disponible");
  }

  LOG_INFO("[INIT] Iniciando Scheduler...");
  smp_init();
  smp_set_bsp_lapic_id(lapic_get_bsp_id());
  sched_init();

  LOG_INFO("[INIT] Process subsystem...");
  extern void process_init(void);
  process_init();

  LOG_INFO("[INIT] Lanzando kswapd...");
  extern void kswapd_main(void);
  task_t *kswapd_t = sched_create_task(kswapd_main);
  if (kswapd_t) {
    LOG_INFO("[INIT] kswapd lanzado (task ID=%u)", kswapd_t->id);
  } else {
    LOG_WARN("[INIT] kswapd no se pudo lanzar (sin memoria)");
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