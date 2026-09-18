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

// Estructura pasada desde el bootloader
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
uint32_t *fb_ptr = NULL;
uint32_t fb_width = 0;
uint32_t fb_height = 0;
uint32_t fb_pitch = 0;

static uint8_t syscall_kernel_stack[8192] __attribute__((aligned(16)));

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

#include "font.h"

static void fb_puts(int x, int y, const char *str, uint32_t fg, uint32_t bg) {
  int cx = x;
  while (*str) {
    unsigned char c = (unsigned char)*str;
    if (c < 128) {
      char *glyph = font8x8_basic[(int)c];
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          uint32_t color = (glyph[row] & (1 << col)) ? fg : bg;
          fb_fillrect(cx + col * 2, y + row * 2, 2, 2, color);
        }
      }
    } else {
      fb_fillrect(cx, y, 16, 16, bg);
    }
    cx += 16;
    str++;
  }
}

// EFI_MEMORY_DESCRIPTOR layout en x86_64:
//   Type(UINT32) @0, Pad(UINT32) @4, PhysicalStart(UINT64) @8,
//   VirtualStart(UINT64) @16, NumberOfPages(UINT64) @24, Attribute(UINT64) @32
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

// PIT: Programmable Interval Timer
#define PIT_FREQ 1193182
#define PIT_HZ 1000

static void pit_init(void) {
  uint16_t divisor = PIT_FREQ / PIT_HZ;
  __asm__ volatile("outb %0, $0x43" : : "a"((uint8_t)0x36));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)(divisor & 0xFF)));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)((divisor >> 8) & 0xFF)));
  LOG_INFO("[PIT] Configurado a %u Hz", PIT_HZ);
}

static inline uint64_t sys_call(uint64_t num, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4, uint64_t arg5) {
  register uint64_t rax __asm__("rax") = num;
  register uint64_t rdi __asm__("rdi") = arg1;
  register uint64_t rsi __asm__("rsi") = arg2;
  register uint64_t rdx __asm__("rdx") = arg3;
  register uint64_t r10 __asm__("r10") = arg4;
  register uint64_t r8 __asm__("r8") = arg5;
  register uint64_t r9 __asm__("r9") = 0;

  __asm__ volatile("syscall"
                   : "+r"(rax)
                   : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return rax;
}

// Inicializar SSE (CR4.OSFXSR)
static void sse_init(void) {
  uint64_t cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= 0x200; // OSFXSR
  cr4 |= 0x400; // OSXMMEXCPT
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));
  LOG_INFO("[SSE] OSFXSR + OSXMMEXCPT habilitados");
}

volatile uint64_t tick_count = 0;
static void timer_handler(void) {
  tick_count++;
  if (tick_count % PIT_HZ == 0) {
    compositor_notify_clock_tick();
  }
  sched_tick(); // Preemptive scheduler
}

// PS/2 keyboard and mouse handled in kernel/ps2.c (ps2_init)

// Servicio IPC en el Kernel para responder peticiones de procesos de usuario
static uint32_t ipc_echo_task_id = 0;
static void ipc_echo_service(void) {
  task_t *self = sched_current();
  ipc_echo_task_id = self->id;
  LOG_INFO("[IPC-KERNEL] Servicio de eco IPC iniciado (Task ID=%u)",
           ipc_echo_task_id);

  while (1) {
    ipc_msg_t msg;
    // Recepción bloqueante
    if (ipc_recv(&msg, 0) == 0) {
      LOG_INFO("[IPC-KERNEL] Mensaje recibido de Tarea ID=%u (tipo=%u, "
               "payload='%s')",
               msg.sender, msg.type, (const char *)msg.data);

      // Responder con un mensaje de confirmacion
      const char *reply = "PONG: Hola desde el Kernel (Ring 0)";
      ipc_send(msg.sender, IPC_TYPE_RESPONSE, reply, 36);
    }
  }
}

void kmain(struct kernel_boot_info *kinfo) {
  // Inicializar serial PRIMERO (antes de cualquier operacion que pueda fallar)
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
      if (end > max_phys_addr) {
        max_phys_addr = end;
      }
    }
  }
  LOG_DEBUG("[INIT] max_phys_addr = %p (%lu MB)", (void *)max_phys_addr,
            (unsigned long)(max_phys_addr / (1024 * 1024)));

  LOG_DEBUG("[DEBUG] boot.memmap=%p size=%lu desc_size=%lu desc_ver=%u",
            (void *)boot.memmap, (unsigned long)boot.memmap_size,
            (unsigned long)boot.memmap_desc_size, boot.memmap_desc_ver);

  if (boot.memmap == 0 || boot.memmap_size == 0 || boot.memmap_desc_size == 0) {
    LOG_INFO(
        "[MEM] Memmap inválido o ausente (0/size/desc_size). Abortando parse.");
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
  // (Los tests de heap ahora se ejecutan vía run_all_tests() más abajo.)

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
    for (size_t i = 0; i < cfg->size; i++) {
      serial_putc(cfg->data[i]);
    }
    serial_puts("'\n");
  }

  LOG_INFO("[INIT] RTC CMOS... ");
  rtc_init();
  LOG_INFO("OK");

  LOG_INFO("[INIT] Iniciando Scheduler...");
  sched_init();
  sched_create_task(ipc_echo_service);

  irq_install_handler(0, timer_handler);

  __asm__ volatile("sti");

  klog_calibrate_tsc(50, 1000);
  LOG_INFO("[TSC] Calibrado a %lu MHz", klog_get_tsc_freq() / 1000000);

  LOG_INFO("[INIT] TTY...");
  tty_init();
  LOG_INFO("OK");

  input_init();

  driver_register(&ps2_driver);
  driver_register(&pci_driver);

  drivers_init_all();

  int fb_ok = 0;
  if (boot.fb_base != 0 && boot.fb_width > 0 && boot.fb_height > 0) {
    LOG_INFO(
        "[FB] Mapeando framebuffer via MMIO_MAP_BASE (Write-Combining)... ");

    void *fb_virt = mmio_map(boot.fb_base, boot.fb_size,
                             PTE_WRITABLE | PTE_WRITECOMB | PTE_NX);
    if (fb_virt) {
      LOG_INFO("OK");
      fb_ok = 1;
      fb_init((uint64_t)fb_virt, boot.fb_width, boot.fb_height, boot.fb_pitch);
    } else {
      LOG_ERR("FALLIDO (mmio_map devolvió NULL)");
    }
  }

  if (!fb_ok) {
    LOG_ERR("[FB] No hay framebuffer disponible o fallo al mapear");
  }

  if (fb_ptr && fb_ok) {
    fb_fillrect(0, 0, fb_width, fb_height, 0x0F0F23);
    fb_fillrect(0, 0, fb_width, 40, 0x1A1A2E);
    fb_puts(10, 12, "Aurora OS", 0xFFFFFF, 0x1A1A2E);

    int win_x = 100, win_y = 80;
    int win_w = 600, win_h = 400;
    fb_fillrect(win_x + 8, win_y + 8, win_w, win_h, 0x000000);
    fb_fillrect(win_x, win_y, win_w, 30, 0x2D2D44);
    fb_fillrect(win_x, win_y + 30, win_w, win_h - 30, 0x1E1E2E);
    fb_puts(win_x + 10, win_y + 8, "Terminal", 0xFFFFFF, 0x2D2D44);
    fb_puts(win_x + 10, win_y + 45, "Aurora OS v0.1.0", 0x00FF88, 0x1E1E2E);
    fb_puts(win_x + 10, win_y + 65, "x86_64 Bare Metal", 0xAAAAAA, 0x1E1E2E);
    fb_puts(win_x + 10, win_y + 85,
            "GDT:OK | IDT:OK | PMM:OK | VMM:OK | HEAP:OK | SCHED:OK", 0xAAAAAA,
            0x1E1E2E);
    fb_puts(win_x + 10, win_y + 105, "Timer: Running | Keyboard: Active",
            0xAAAAAA, 0x1E1E2E);

    uint64_t test_page = pmm_alloc_page();
    if (test_page) {
      LOG_INFO("[PMM-TEST] Pagina reservada en %p", (void *)test_page);
      pmm_free_page(test_page);
    }

    fb_puts(win_x + 10, win_y + 145, "> _", 0x00FF88, 0x1E1E2E);
    LOG_INFO("[INIT] Entorno grafico inicializado");
  }

  LOG_INFO("[INIT] Aurora OS listo. Multitarea activa.");
  if (fb_ok) {
    compositor_init();

    LOG_INFO("[KERNEL] Ventanas creadas. Cediendo control al compositor...");
    sched_create_task(compositor_thread);
  }

  // ---------------------------------------------------------------------------
  // TEST SUITE — E2
  //
  // Corre todos los tests registrados en la sección .tests.
  // Corre con preemption habilitada. Si algún test falla, se imprime
  // un resumen al final pero el kernel sigue arrancando.
  // ---------------------------------------------------------------------------
  int failed = run_all_tests();
  if (failed > 0) {
    LOG_ERR("[TEST] %d tests fallaron. Revisar arriba.", failed);
  }

  LOG_INFO("[INIT] Cargando shell interactivo 'apps/shell'...");
  process_load("apps/shell");

  LOG_DEBUG("[KERNEL] kmain: cediendo control al scheduler");
  while (1) {
    sched_yield();
    __asm__ volatile("hlt");
  }
}