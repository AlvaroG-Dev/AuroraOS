#include "ipc.h"
// kernel/main.c
// Punto de entrada del kernel Aurora OS

#include "cpu.h"
#include "driver.h"
#include "gdt.h"
#include "gfx/compositor.h"
#include "gfx/theme.h"
#include "heap.h"
#include "idt.h"
#include "initrd.h"
#include "input.h"
#include "klog.h"
#include "paging.h"
#include "pci.h"
#include "pf.h"
#include "pmm.h"
#include "process.h"
#include "ps2.h"
#include "rtc.h"
#include "sched.h"
#include "serial.h"
#include "string.h"
#include "syscall.h"
#include "tarfs.h"
#include "test.h"
#include "tty.h"
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Boot info
// ---------------------------------------------------------------------------
struct kernel_boot_info {
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
};

static struct kernel_boot_info boot;
static struct kernel_boot_info g_boot_info; // copia para kmain_task

uint32_t *fb_ptr = NULL;
uint32_t fb_width = 0;
uint32_t fb_height = 0;
uint32_t fb_pitch = 0;

static uint8_t syscall_kernel_stack[8192] __attribute__((aligned(16)));

// ---------------------------------------------------------------------------
// Framebuffer helpers (solo se usan en kmain_task, tras inicializar la FB)
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

static void fb_putpixel(int x, int y, uint32_t color) {
  if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height)
    return;
  fb_ptr[y * fb_pitch + x] = color;
}

static void fb_fillrect(int x, int y, int w, int h, uint32_t color) {
  for (int row = y; row < y + h && row < (int)fb_height; row++) {
    for (int col = x; col < x + w && col < (int)fb_width; col++) {
      fb_putpixel(col, row, color);
    }
  }
}

// ---------------------------------------------------------------------------
// Memmap parse (solo en kmain, antes de paging_init)
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
// PIT
// ---------------------------------------------------------------------------
#define PIT_FREQ 1193182
#define PIT_HZ 1000

static void pit_init(void) {
  uint16_t divisor = PIT_FREQ / PIT_HZ;
  __asm__ volatile("outb %0, $0x43" : : "a"((uint8_t)0x36));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)(divisor & 0xFF)));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)((divisor >> 8) & 0xFF)));
  LOG_INFO("[PIT] Configurado a %u Hz", PIT_HZ);
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
// Timer handler
// ---------------------------------------------------------------------------
volatile uint64_t tick_count = 0;
static void timer_handler(void) {
  tick_count++;
  if (tick_count % PIT_HZ == 0) {
    compositor_notify_clock_tick();
  }
  sched_tick();
}

// ---------------------------------------------------------------------------
// Servicio IPC de eco (tarea del kernel)
// ---------------------------------------------------------------------------
static uint32_t ipc_echo_task_id = 0;
static void ipc_echo_service(void) {
  task_t *self = sched_current();
  ipc_echo_task_id = self->id;
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
// kmain_task: aquí corre TODO lo que necesita interrupciones activas
// o drivers inicializados. Es una tarea real del scheduler.
// ===========================================================================
static void kmain_task(void) {
  LOG_INFO("[KERNEL] kmain_task: inicio (id=%u)", sched_current()->id);

  // La primera tarea en arrancar tras la idle. Creamos las demás.
  sched_create_task(ipc_echo_service);

  // Instalar el handler del timer y habilitar interrupciones.
  irq_install_handler(0, timer_handler);
  __asm__ volatile("sti");

  klog_calibrate_tsc(50, 1000);
  LOG_INFO("[TSC] Calibrado a %lu MHz", klog_get_tsc_freq() / 1000000);

  // TTY.
  LOG_INFO("[INIT] TTY...");
  tty_init();
  LOG_INFO("OK");

  input_init();

  driver_register(&ps2_driver);
  driver_register(&pci_driver);
  drivers_init_all();

  // Framebuffer.
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

  // Tests del kernel.
  int failed = run_all_tests();
  if (failed > 0) {
    LOG_ERR("[TEST] %d tests fallaron. Revisar arriba.", failed);
  }

  smp_dump();

  // App de consola gráfica.
  LOG_INFO("[INIT] Cargando shell interactivo 'apps/shell'...");
  process_load("apps/shell");

  // kmain_task ya ha hecho todo. Cede para siempre.
  // La idle se encarga del hlt cuando no hay nada más.
  while (1) {
    sched_yield();
  }
}

// ===========================================================================
// kmain: setup mínimo que NO necesita interrupciones activas.
// Tras crear la tarea kmain_task, cede el control y no vuelve.
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

  memcpy(&boot, kinfo, sizeof(boot));
  // Copia para kmain_task. `boot` en sí mismo no se toca más tras esto.
  g_boot_info = boot;

  LOG_INFO("[BOOT] Framebuffer: %p %u x %u pitch=%u", (void *)boot.fb_base,
           boot.fb_width, boot.fb_height, boot.fb_pitch);

  LOG_INFO("[INIT] GDT... ");
  gdt_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] IDT... ");
  idt_init();
  LOG_INFO("OK");

  cpu_local.kernel_stack =
      (uint64_t)(syscall_kernel_stack + sizeof(syscall_kernel_stack));
  cpu_local.user_rsp = 0;

  LOG_INFO("[INIT] Syscalls... ");
  syscall_init();
  LOG_INFO("OK");

  // Calcular max_phys_addr.
  uint64_t max_phys_addr = 0;
  if (boot.memmap && boot.memmap_size > 0) {
    uint8_t *ptr = (uint8_t *)boot.memmap;
    for (uint64_t i = 0; i < boot.memmap_size; i += boot.memmap_desc_size) {
      uint32_t type = *(uint32_t *)(ptr + i + 0);
      if (type != EFI_CONVENTIONAL_MEMORY)
        continue;
      uint64_t phys = *(uint64_t *)(ptr + i + 8);
      uint64_t pages = *(uint64_t *)(ptr + i + 24);
      uint64_t end = phys + (pages * PAGE_SIZE);
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

  extern void pmm_self_test(void);
  pmm_self_test();

  LOG_INFO("[INIT] Paging... ");
  uint64_t cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  paging_init((uint64_t *)cr3, max_phys_addr);
  LOG_INFO("OK");

  pmm_relocate_bitmap();

  LOG_INFO("[INIT] Demand paging (PF handler)...");
  pf_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Iniciando Heap (kmalloc)...");
  heap_init();

  pit_init();

  LOG_INFO("[INIT] Inicializando Initramfs (TarFS)...");
  size_t initrd_size = (size_t)(initrd_end - initrd_start);
  tarfs_init(initrd_start, initrd_size);
  vfs_init();
  ipc_init();
  tarfs_list("");
  tar_node_t *cfg = tarfs_open("system/config.txt");
  if (cfg) {
    serial_puts("[TARFS] Contenido de system/config.txt:\n    '");
    for (size_t i = 0; i < cfg->size; i++)
      serial_putc(cfg->data[i]);
    serial_puts("'\n");
  }

  LOG_INFO("[INIT] RTC CMOS... ");
  rtc_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Iniciando Scheduler...");
  sched_init();
  smp_init();

  // Crear la tarea kmain_task. NO la ejecutamos todavía.
  task_t *t = sched_create_task(kmain_task);
  if (!t) {
    LOG_PANIC("[KERNEL] no se pudo crear kmain_task");
  }

  // Saltar directamente a kmain_task. Esto NO retorna. La idle queda
  // intacta, con su contexto armado, esperando a que el scheduler la
  // elija por primera vez.
  LOG_DEBUG("[KERNEL] kmain: saltando a kmain_task");
  sched_start(t);

  // No se llega aquí.
  __builtin_unreachable();
}