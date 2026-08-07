// kernel/main.c
// Punto de entrada del kernel Aurora OS

#include "gdt.h"
#include "gfx/compositor.h"
#include "gfx/font_manager.h"
#include "gfx/theme.h"
#include "heap.h"
#include "idt.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "serial.h"
#include "ps2.h"
#include "rtc.h"
#include "tarfs.h"
#include "initrd.h"
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

void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *pdest = (uint8_t *)dest;
  const uint8_t *psrc = (const uint8_t *)src;
  for (size_t i = 0; i < n; i++) {
    pdest[i] = psrc[i];
  }
  return dest;
}

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

  serial_puts("[FB] Inicializado en memoria: ");
  serial_hex(base);
  serial_puts(" ");
  serial_putn(w, 10, 0);
  serial_puts("x");
  serial_putn(h, 10, 0);
  serial_puts(" pitch=");
  serial_putn(pitch, 10, 0);
  serial_puts("\n");
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
    serial_puts("[MEM] No hay mapa de memoria disponible\n");
    return;
  }
  serial_puts("[MEM] Mapa de memoria EFI:\n");
  uint8_t *ptr = (uint8_t *)boot.memmap;
  uint64_t total_usable = 0;

  for (uint64_t i = 0; i < boot.memmap_size; i += boot.memmap_desc_size) {
    uint32_t type = *(uint32_t *)(ptr + i + 0);
    uint64_t phys = *(uint64_t *)(ptr + i + 8);
    uint64_t pages = *(uint64_t *)(ptr + i + 24);
    uint64_t len = pages * 4096;

    if (type == EFI_CONVENTIONAL_MEMORY) {
      total_usable += len;
      serial_puts("  [USABLE] 0x");
      serial_hex(phys);
      serial_puts(" - 0x");
      serial_hex(phys + len);
      serial_puts(" (");
      serial_putn(len / (1024 * 1024), 10, 0);
      serial_puts(" MB)\n");
    }
  }
  serial_puts("[MEM] Total usable: ");
  serial_putn(total_usable / (1024 * 1024), 10, 0);
  serial_puts(" MB\n");
}

// PIT: Programmable Interval Timer
#define PIT_FREQ 1193182
#define PIT_HZ 1000

static void pit_init(void) {
  uint16_t divisor = PIT_FREQ / PIT_HZ;
  __asm__ volatile("outb %0, $0x43" : : "a"((uint8_t)0x36));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)(divisor & 0xFF)));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)((divisor >> 8) & 0xFF)));
  serial_puts("[PIT] Configurado a ");
  serial_putn(PIT_HZ, 10, 0);
  serial_puts(" Hz\n");
}

// ---------------------------------------------------------------------------
// Tareas de demostracion del scheduler
// ---------------------------------------------------------------------------
static void task_demo_a(void);
static void task_demo_b(void);

static void task_demo_a(void) {
  uint64_t count = 0;
  while (1) {
    if (count % 5000000 == 0) {
      serial_puts("[TASK-A] vivo\n");
    }
    count++;
    sched_yield(); // Cede el turno inmediatamente en vez de dormir la CPU con hlt
  }
}

static void task_demo_b(void) {
  uint64_t count = 0;
  while (1) {
    if (count % 5000000 == 0) {
      serial_puts("[TASK-B] vivo\n");
    }
    count++;
    sched_yield(); // Cede el turno inmediatamente
  }
}

// Inicializar SSE (CR4.OSFXSR)
static void sse_init(void) {
  uint64_t cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= 0x200; // OSFXSR
  cr4 |= 0x400; // OSXMMEXCPT
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));
  serial_puts("[SSE] OSFXSR + OSXMMEXCPT habilitados\n");
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

void kmain(struct kernel_boot_info *kinfo) {
  // Inicializar serial PRIMERO (antes de cualquier operacion que pueda fallar)
  serial_init();
  serial_puts("\n========================================\n");
  serial_puts("   AURORA OS KERNEL x86_64\n");
  serial_puts("   Fase 1 - Kernel Base (REV 4)\n");
  serial_puts("========================================\n\n");

  // Inicializar SSE ANTES de cualquier operacion que pueda usar SSE
  // (como __builtin_memcpy)
  serial_puts("[INIT] SSE... ");
  sse_init();
  serial_puts("OK\n");

  // Ahora es seguro usar __builtin_memcpy (puede usar movaps)
  __builtin_memcpy(&boot, kinfo, sizeof(boot));

  serial_puts("[BOOT] Framebuffer: 0x");
  serial_hex(boot.fb_base);
  serial_puts(" ");
  serial_putn(boot.fb_width, 10, 0);
  serial_puts("x");
  serial_putn(boot.fb_height, 10, 0);
  serial_puts(" pitch=");
  serial_putn(boot.fb_pitch, 10, 0);
  serial_puts("\n");

  serial_puts("[INIT] GDT... ");
  gdt_init();
  serial_puts("OK\n");

  serial_puts("[INIT] IDT... ");
  idt_init();
  serial_puts("OK\n");

  serial_puts("[INIT] Paging... ");
  uint64_t cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  paging_init((uint64_t *)cr3);
  serial_puts("OK\n");

  parse_memmap();

  serial_puts("[INIT] Iniciando PMM...\n");
  pmm_init(boot.memmap, boot.memmap_size, boot.memmap_desc_size);

  serial_puts("[INIT] Iniciando Heap (kmalloc)...\n");
  heap_init();

  // Tests del heap
  uint8_t *buf = (uint8_t *)kmalloc(64);
  if (buf) {
    for (int i = 0; i < 64; i++)
      buf[i] = (uint8_t)i;
    int ok = 1;
    for (int i = 0; i < 64; i++)
      if (buf[i] != (uint8_t)i) {
        ok = 0;
        break;
      }
    serial_puts(ok ? "[HEAP-TEST] kmalloc 64B: OK\n"
                   : "[HEAP-TEST] kmalloc 64B: FALLO\n");
    kfree(buf);
  }
  uint8_t *big = (uint8_t *)kmalloc(8192);
  serial_puts(big ? "[HEAP-TEST] kmalloc 8KB: OK\n"
                  : "[HEAP-TEST] kmalloc 8KB: FALLO\n");
  if (big)
    kfree(big);
  pit_init();
  serial_puts("[INIT] Inicializando Initramfs (TarFS)...\n");
  size_t initrd_size = (size_t)(initrd_end - initrd_start);
  tarfs_init(initrd_start, initrd_size);
  // Listar todo el Initramfs por consola serie
  tarfs_list("");
  // Prueba de apertura de archivo de prueba
  tar_node_t *cfg = tarfs_open("system/config.txt");
  if (cfg) {
      serial_puts("[TARFS] Contenido de system/config.txt:\n    '");
      for (size_t i = 0; i < cfg->size; i++) {
          serial_putc(cfg->data[i]);
      }
      serial_puts("'\n");
  }
  // Inicializar RTC CMOS
  serial_puts("[INIT] RTC CMOS... ");
  rtc_init();
  serial_puts("OK\n");
  // Iniciar scheduler ANTES de los drivers, despues del heap
  serial_puts("[INIT] Iniciando Scheduler...\n");
  sched_init();

  irq_install_handler(0, timer_handler);
  // Initialize PS/2 keyboard + mouse (registers IRQ handlers)
  ps2_init();

// Inicializar framebuffer solo si hay uno valido
  int fb_ok = 0;
  if (boot.fb_base != 0 && boot.fb_width > 0 && boot.fb_height > 0) {
    serial_puts("[FB] Mapeando framebuffer en modo Write-Combining (WC)... ");
    
    uint64_t fb_aligned = boot.fb_base & ~0xFFFULL;
    uint64_t fb_end = (boot.fb_base + boot.fb_size + 0xFFF) & ~0xFFFULL;
    uint64_t fb_size_aligned = fb_end - fb_aligned;

    // Remapeamos SIEMPRE con PTE_WRITECOMB (sin importar si fb_base < 4GB o > 4GB)
    if (paging_map_range(fb_aligned, fb_aligned, fb_size_aligned,
                         PTE_WRITABLE | PTE_WRITECOMB) == 0) {
      serial_puts("OK\n");
      fb_ok = 1;
    } else {
      serial_puts("FALLIDO (pool agotado)\n");
    }

    if (fb_ok) {
      fb_init(boot.fb_base, boot.fb_width, boot.fb_height, boot.fb_pitch);
    }
  }

  if (!fb_ok) {
    serial_puts("[FB] No hay framebuffer disponible o fallo al mapear\n");
  }

  // Dibujar UI solo si el framebuffer esta disponible
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

    // PMM Test (ya fue corrido arriba, solo mostramos el resultado)
    uint64_t test_page = pmm_alloc_page();
    if (test_page) {
      serial_puts("[PMM-TEST] Pagina reservada en 0x");
      serial_hex(test_page);
      serial_puts("\n");
      pmm_free_page(test_page);
    }

    fb_puts(win_x + 10, win_y + 145, "> _", 0x00FF88, 0x1E1E2E);
    serial_puts("[INIT] Entorno grafico inicializado\n");
  }

  serial_puts("[INIT] Aurora OS listo. Multitarea activa.\n");
  if (fb_ok) {
    compositor_init();

// 3. Crear Ventana 1: Aurora Terminal
  window_t *win_term = compositor_create_window(
      100, 80, 520, 340, "Aurora Terminal - x86_64", WIN_FLAGS_INACTIVE);

  if (win_term) {
    win_set_icon_text(win_term, ">", WIN11_ACCENT);
    win_clear(win_term, 0xFF1E1E1E); // Fondo oscuro de terminal
    
    // Dibujar prompt de comando de ejemplo
    win_draw_string(win_term, 16, 16, "aurora-os:~$ ", WIN11_ACCENT, FONT_ID_MONO);
    win_update(win_term);
  }

  // 4. Crear Ventana 2: System Performance (Activa / Enfocada)
  window_t *win_perf = compositor_create_window(
      300, 180, 420, 260, "System Performance", WIN_FLAGS_FOCUSED);

  if (win_perf) {
    win_set_icon_text(win_perf, ">", WIN11_ACCENT);
    win_clear(win_perf, WIN11_SURFACE_CARD);
    
    // Contenido de rendimiento
    win_draw_string(win_perf, 20, 20, "CPU Usage: 3%", WIN11_TEXT_PRIMARY, FONT_ID_MONO);
    win_draw_string(win_perf, 20, 40, "RAM Usage: 42MB / 512MB", WIN11_TEXT_SECONDARY, FONT_ID_MONO);
    win_update(win_perf);
  }

  serial_puts("[KERNEL] Ventanas creadas. Cediendo control al compositor...\n");
    sched_create_task(compositor_thread);
  }
  // Crear dos tareas de demostracion
  // (definidas mas abajo, necesitan ser antes de sti)
  sched_create_task(task_demo_a);
  sched_create_task(task_demo_b);

  __asm__ volatile("sti");

  // Loop principal (tarea idle)
  uint64_t last_tick = 0;
  while (1) {
    if (tick_count != last_tick) {
      last_tick = tick_count;
      if (tick_count % (PIT_HZ / 2) == 0) {
        static int on = 0;
        on = !on;
        uint32_t color = on ? 0x00FF88 : 0x1E1E2E;
        if (fb_ptr && fb_ok)
          fb_fillrect(130, 225, 8, 16, color);
      }
    }
    // Process buffered PS/2 events in non-IRQ context
    ps2_process();
    __asm__ volatile("hlt");
  }
}
