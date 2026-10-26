// kernel/main.c
// Punto de entrada del kernel Aurora OS.

#include "../bootloader/include/kernel_boot_info.h"
#include "acpi.h"
#include "acpi_aml.h"
#include "acpi_pm.h"
#include "ahci.h"
#include "apic.h"
#include "ata_common.h"
#include "ata_dma.h"
#include "atapi.h"
#include "aur_bus.h"
#include "block.h"
#include "block_proxy.h"
#include "gpu_proxy.h"
#include "boot_console.h"
#include "boot_splash.h"
#include "bus.h"
#include "completion.h"
#include "cpu.h"
#include "driver.h"
#include "elf.h"
#include "fat32.h"
#include "gdt.h"
#include "gfx/compositor.h"
#include "gfx/theme.h"
#include "heap.h"
#include "idt.h"
#include "evdev.h"
#include "drm/drm.h"
#include "input.h"
#include "ipc.h"
#include "ipi.h"
#include "irq_threaded.h"
#include "iso9660.h"
#include "klog.h"
#include "ksymtab.h"
#include "kthread.h"
#include "kur_loader.h"
#include "net/arp.h"
#include "net/dhcp.h"
#include "net/e1000e.h"
#include "net/icmp.h"
#include "net/ip.h"
#include "net/loopback.h"
#include "net/net_proxy.h"
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
#include "workqueue.h"
#include <stddef.h>
#include <stdint.h>

// ===========================================================================
// Estado global (accesible desde tareas de init)
// ===========================================================================
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

// Completions globales. Reglas de consumidor:
//   - g_acpi_done:   0 waiters (informativo)
//   - g_fb_done:     1 waiter  → complete()
//   - g_vfs_done:    3 waiters → complete_all()   [ver fix #1]
//   - g_drivers_done: 1 waiter → complete()
//   - g_smp_done:    1 waiter  → complete()
//   - g_timer_done:  0 waiters (informativo)
static completion_t g_acpi_done;
static completion_t g_fb_done;
static completion_t g_vfs_done;
static completion_t g_drivers_done;
static completion_t g_smp_done;
static completion_t g_timer_done;

// ===========================================================================
// Framebuffer helpers
// ===========================================================================
static int fb_init(uint64_t base, uint32_t w, uint32_t h, uint32_t pitch,
                   uint32_t bpp, uint64_t size) {
  if (base == 0 || w == 0 || h == 0 || pitch == 0 || (bpp != 24 && bpp != 32) ||
      pitch < (uint64_t)w * (bpp / 8) || (uint64_t)pitch * h > size) {
    fb_ptr = NULL;
    fb_hw_ptr = NULL;
    fb_width = fb_height = fb_pitch = fb_bpp = 0;
    fb_hw_pitch = 0;
    LOG_WARN("[FB] parametros invalidos");
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
      return 0;
    }
    fb_ptr = (uint32_t *)kmalloc((size_t)pixels * sizeof(uint32_t));
    if (!fb_ptr) {
      fb_hw_ptr = NULL;
      return 0;
    }
  } else {
    fb_ptr = (uint32_t *)base;
  }
  LOG_INFO("[FB] Inicializado: %p %ux%u bpp=%u", (void *)base, w, h, bpp);
  return 1;
}

// Pantalla "tardia" (ver gpu_proxy.c): framebuffer en RAM de un driver GPU.
int fb_adopt_ram(uint64_t phys, uint64_t size, uint32_t w, uint32_t h,
                 uint32_t pitch) {
  if (fb_ptr)
    return -EBUSY;
  if (!fb_init((uint64_t)phys_to_virt(phys), w, h, pitch, 32, size))
    return -EINVAL;
  gpup_set_framebuffer(phys, size, w, h, pitch, 32);
  boot_console_init(fb_ptr, fb_width, fb_height, fb_pitch);
  boot_console_clear();
  boot_console_hook_install();
  return 0;
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

// ===========================================================================
// Memmap helpers
// ===========================================================================
#define EFI_CONVENTIONAL_MEMORY 7

static void parse_memmap(void) {
  if (!boot.memmap || boot.memmap_size == 0 || boot.memmap_desc_size < 40 ||
      boot.memmap_size % boot.memmap_desc_size != 0)
    return;
  uint8_t *ptr = (uint8_t *)boot.memmap;
  uint64_t total = 0;
  for (uint64_t i = 0; i < boot.memmap_size; i += boot.memmap_desc_size) {
    uint32_t type = *(uint32_t *)(ptr + i + 0);
    uint64_t pages = *(uint64_t *)(ptr + i + 24);
    uint64_t len = pages * 4096;
    if (type == EFI_CONVENTIONAL_MEMORY)
      total += len;
  }
  LOG_INFO("[MEM] Total usable: %lu MB",
           (unsigned long)(total / (1024 * 1024)));
}

// ===========================================================================
// SSE
// ===========================================================================
static void sse_init(void) {
  uint64_t cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= 0x200;
  cr4 |= 0x400;
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));
  LOG_INFO("[SSE] OSFXSR + OSXMMEXCPT habilitados");
}

// ===========================================================================
// Servicio IPC de eco
// ===========================================================================
static uint32_t ipc_echo_task_id = 0;

static void ipc_echo_service(void) {
  task_t *self = sched_current();
  ipc_echo_task_id = self->id;
  syscall_register_service("echo", self->id);
  LOG_INFO("[IPC-KERNEL] Servicio de eco iniciado (Task ID=%u)",
           ipc_echo_task_id);
  while (1) {
    ipc_msg_t msg;
    if (ipc_recv(&msg, 0) == 0) {
      const char *reply = "PONG: Hola desde el Kernel (Ring 0)";
      ipc_send(msg.sender, IPC_TYPE_RESPONSE, reply, 36);
    }
  }
}

// ===========================================================================
// INIT WORKERS
//
// Reglas comunes:
//   - Usan SOLO LOG_INFO/LOG_WARN/LOG_ERR (thread-safe). NO klog_boot_step
//     ni klog_boot_ok: esos pares mantienen estado global y con varios
//     workers concurrentes se pisan entre sí (fix #5).
//   - Reciben la completion como void* arg y la completan al terminar.
//   - Si esperan a otra completion, usan wait_for_completion_timeout para
//     no colgarse si la otra falla.
// ===========================================================================

// ---------------------------------------------------------------------------
// Timer: TSC + LAPIC. Síncrono (no kthread), lo hace system_init_task.
// ---------------------------------------------------------------------------
static int timer_init_task(void *arg) {
  completion_t *done = (completion_t *)arg;
  LOG_INFO("[INIT] TSC + LAPIC timer...");

  klog_calibrate_tsc(50, KERNEL_HZ);
  boot_splash_set_tsc_hz(klog_get_tsc_freq());
  lapic_timer_init();

  LOG_INFO("[INIT] Timer listo (%lu MHz)", klog_get_tsc_freq() / 1000000);
  if (done)
    complete(done);
  return 0;
}

// ---------------------------------------------------------------------------
// ACPI AML + PM. Single-consumer completion.
// ---------------------------------------------------------------------------
static int acpi_init_task(void *arg) {
  completion_t *done = (completion_t *)arg;

  LOG_INFO("[INIT] ACPI AML (namespace)...");
  aml_init();

  LOG_INFO("[INIT] ACPI PM (P-states, S-states)...");
  acpi_pm_init();

  LOG_INFO("[INIT] PM: aplicar política...");
  cpu_apply_pm_policy();

  if (done)
    complete(done);
  return 0;
}

// ---------------------------------------------------------------------------
// Framebuffer + splash. Single-consumer completion.
// ---------------------------------------------------------------------------
static int fb_init_task(void *arg) {
  completion_t *done = (completion_t *)arg;

  LOG_INFO("[INIT] Framebuffer...");

  if (boot.fb_base && boot.fb_width > 0 && boot.fb_height > 0 &&
      boot.fb_pitch > 0 && boot.fb_size > 0) {
    void *fb_virt = mmio_map(boot.fb_base, boot.fb_size,
                             PTE_WRITABLE | PTE_WRITECOMB | PTE_NX);
    if (fb_virt && fb_init((uint64_t)fb_virt, boot.fb_width, boot.fb_height,
                           boot.fb_pitch, boot.fb_bpp, boot.fb_size)) {
      gpup_set_framebuffer(boot.fb_base, boot.fb_size, boot.fb_width,
                           boot.fb_height, boot.fb_pitch, boot.fb_bpp);
      if (g_boot_splash_enabled) {
        extern uint32_t g_boot_splash_initial_phase;
        extern uint32_t g_boot_splash_initial_ms;
        g_boot_splash_initial_phase = boot.splash_phase;
        g_boot_splash_initial_ms = boot.splash_elapsed_ms;
        if (boot_splash_init(fb_ptr, fb_width, fb_height, fb_pitch) == 0)
          LOG_INFO("[FB] Boot splash activa (%ux%u)", fb_width, fb_height);
        else {
          boot_console_init(fb_ptr, fb_width, fb_height, fb_pitch);
          boot_console_clear();
          boot_console_hook_install();
        }
      } else {
        boot_console_init(fb_ptr, fb_width, fb_height, fb_pitch);
        boot_console_clear();
        boot_console_hook_install();
      }
    }
  } else {
    LOG_WARN("[FB] sin framebuffer disponible");
  }

  // Tarea de animación del splash.
  if (g_boot_splash_enabled && boot_splash_active()) {
    task_t *splash_t = sched_create_task(boot_splash_task_loop);
    if (splash_t)
      splash_t->cpu_affinity = 0;
    else
      LOG_WARN("[SPLASH] no se pudo crear splash task");
  }

  if (done)
    complete(done);
  return 0;
}

// ---------------------------------------------------------------------------
// VFS + servicios de usuario.
//
// [FIX #1] complete_all(): 3 waiters concurrentes (system_init_task,
// drivers_init_task, net_init_task) esperan en esta completion. Usar
// complete() despertaría a UNO y dejaría a los otros dos bloqueados
// hasta su timeout (bug del boot anterior: 8.5 s perdidos).
// ---------------------------------------------------------------------------
static int vfs_init_task(void *arg) {
  completion_t *done = (completion_t *)arg;

  LOG_INFO("[INIT] Initramfs (TarFS)...");
  if (boot.initrd_base == 0 || boot.initrd_size == 0) {
    LOG_WARN("[INIT] initrd no cargado");
  } else {
    const void *initrd_virt = phys_to_virt(boot.initrd_base);
    LOG_INFO("[INIT] initrd en phys=0x%lx (%lu KB)",
             (unsigned long)boot.initrd_base,
             (unsigned long)(boot.initrd_size / 1024));
    tarfs_init(initrd_virt, (size_t)boot.initrd_size);
  }

  LOG_INFO("[INIT] VFS...");
  vfs_init();

  LOG_INFO("[INIT] IPC...");
  ipc_init();

  LOG_INFO("[INIT] RTC CMOS...");
  rtc_init();

  LOG_INFO("[INIT] vDSO...");
  vdso_init();

  LOG_INFO("[INIT] TTY...");
  tty_init();

  LOG_INFO("[INIT] PTY pool...");
  pty_init();

  // kswapd (necesita swap_init de kmain + sched).
  extern void kswapd_main(void);
  task_t *kt = sched_create_task(kswapd_main);
  if (kt)
    LOG_INFO("[INIT] kswapd lanzado (task ID=%u)", kt->id);
  else
    LOG_WARN("[INIT] kswapd no se pudo lanzar");

  // [FASE 3.3] Cargar módulos .kur desde /lib/modules (dentro del
  // initrd, montado como tarfs). Así el mismo binario .kur sirve para
  // tests y producción, y se puede inspeccionar con `ls`/`hexdump`
  // desde userland. NO se usan blobs embebidos.
  LOG_INFO("[INIT] Cargando módulos .kur desde /lib/modules...");
  extern int kur_load_all(const char *dir);
  int kur_loaded = kur_load_all("/lib/modules");
  if (kur_loaded < 0) {
    LOG_WARN("[INIT] kur_load_all('/lib/modules') falló: %d", kur_loaded);
  } else {
    LOG_INFO("[INIT] %d módulo(s) .kur cargado(s)", kur_loaded);
  }

  // [FIX #1] complete_all, NO complete.
  if (done)
    complete_all(done);
  return 0;
}

// ---------------------------------------------------------------------------
// Drivers: block layer + input + bus.
//
// Espera a VFS (con timeout por si falla), pero ahora VFS señaliza con
// complete_all() y drivers despierta en milisegundos, no tras timeout.
// ---------------------------------------------------------------------------
static int drivers_init_task(void *arg) {
  completion_t *done = (completion_t *)arg;

  if (wait_for_completion_timeout(&g_vfs_done, 5000) == 0)
    LOG_WARN("[INIT] VFS timeout, drivers pueden fallar");

  LOG_INFO("[INIT] Block layer...");
  blk_init();

  LOG_INFO("[INIT] Input...");
  input_init();
  evdev_init();
  drm_init();

  LOG_INFO("[INIT] Buses...");
  bus_register(&pci_bus); // NEW: idempotente, explícito

  LOG_INFO("[INIT] Drivers bus-managed...");

  // [FASE 4.7] Registrar el bus de módulos de usuario PRIMERO.
  // Al usar una lista LIFO en bus.c, esto asegura que se pruebe
  // AL FINAL, dejando que los drivers del kernel (ahci, xhci)
  // tengan prioridad.
  driver_register(&aur_bus_driver); // <-- AÑADIR ESTA LÍNEA

  driver_register(&ahci_driver); // NEW: va por bus_add_driver
  driver_register(&xhci_driver); // NEW

  LOG_INFO("[INIT] Drivers legacy...");
  driver_register(&ps2_driver);
  driver_register(&pci_driver); // scanner, legacy (tiene .init)
  driver_register(&ata_pio_driver);
  driver_register(&atapi_driver);

  drivers_init_all_parallel(); // solo legacy (bus-managed ya está)
  klog_boot_ok();

  extern int ata_pio_finalize(void);
  ata_pio_finalize();

  if (done)
    complete(done);
  return 0;
}

// ---------------------------------------------------------------------------
// Red: stack IP + NIC.
//
// [FIX #3] Solo inicializa el stack y registra la NIC. NO arranca DHCP
// ni ping: eso se hace en system_init_task DESPUÉS de run_all_tests()
// para que los tests de e1000e vean netif->ip == 0.
//
// e1000e_start() tampoco se llama aquí: sin DHCP no hay paquetes que
// procesar, y arrancar el RX kthread antes de tests podría procesar
// ARPs espontáneos si QEMU emite alguno (no debería, pero mejor
// prevenir).
// ---------------------------------------------------------------------------
static int net_init_task(void *arg) {
  (void)arg;
  if (wait_for_completion_timeout(&g_vfs_done, 5000) == 0)
    LOG_WARN("[INIT] VFS timeout, red puede fallar");

  LOG_INFO("[INIT] Networking...");
  net_init();
  loopback_init();
  ip_init();
  arp_init();
  icmp_init();
  udp_init();
  socket_subsystem_init();
  tcp_init();

  LOG_INFO("[INIT] Virtual Net Proxy (TUN/TAP)...");
  net_proxy_init();

  // No completion: system_init_task no espera a net-init directamente.
  return 0;
}

// ---------------------------------------------------------------------------
// SMP: arranque de APs.
//
// [FIX #6] smp_dump() y apic_dump() emiten ~3 s de log. Compilamos
// condicionalmente con -D SMP_VERBOSE_DUMP.
// ---------------------------------------------------------------------------
static int smp_boot_task(void *arg) {
  completion_t *done = (completion_t *)arg;

  preempt_disable();
  smp_boot_init();
  smp_boot_aps();
  __atomic_store_n(&smp_sched_active, 1, __ATOMIC_RELEASE);
  preempt_enable();

#ifdef SMP_VERBOSE_DUMP
  smp_dump();
  apic_dump();
#endif

  if (done)
    complete(done);
  return 0;
}

// ---------------------------------------------------------------------------
// [REFACTOR] Tests en kthread separado.
//
// Antes: system_init_task llamaba run_all_tests() inline. Bloqueaba ~1.6 s
// antes de cargar /usr/bin/init.
//
// Ahora: kthread propio. /usr/bin/init arranca inmediatamente en paralelo.
//
// IMPORTANTE (red de usuario): DHCP/ping modifican netif->ip. Los tests
// de e1000e asumen netif->ip == 0. Por eso DHCP/ping arrancan al FINAL
// de este kthread, no en system_init_task.
// ---------------------------------------------------------------------------
static void tests_thread(void) {
  LOG_INFO("[TEST] kthread tests iniciado (id=%u)", sched_current()->id);

  extern void __sched_canary_arm(void);
  extern void __sched_canary_check(void);
  __sched_canary_arm();

  int failed = run_all_tests();
  if (failed > 0)
    LOG_ERR("[TEST] %d tests fallaron", failed);

  __sched_canary_check();

  LOG_INFO("[TEST] kthread tests terminado (id=%u, failed=%d)",
           sched_current()->id, failed);

  // [FASE 3.3] Los .kur se cargaron desde VFS en vfs_init_task. Este
  // test verifica que están presentes, que hello2 importa de hello
  // (n_dependents) y que el ciclo unload funciona. NO carga desde
  // blobs.
  {
    int modules_before = kur_loader_count();
    int expected_after = modules_before;
    struct kur_module *m1 = kur_loader_find_by_name("hello");
    struct kur_module *m2 = kur_loader_find_by_name("hello2");

    LOG_INFO("[TEST-KUR2] hello loaded=%d hello2 loaded=%d (esperado 1 1)",
             m1 != NULL, m2 != NULL);
    if (m1)
      LOG_INFO("[TEST-KUR2] hello.n_dependents=%u (esperado 1)",
               m1->n_dependents);
    if (m2)
      LOG_INFO("[TEST-KUR2] hello2.n_dependents=%u (esperado 0)",
               m2->n_dependents);

    if (m1 && m2) {
      int urc1 = kur_loader_unload(m1);
      LOG_INFO("[TEST-KUR2] unload(hello) con hello2 vivo = %d "
               "(esperado -16)",
               urc1);
      int urc2 = kur_loader_unload(m2);
      LOG_INFO("[TEST-KUR2] unload(hello2) = %d (esperado 0)", urc2);
      if (urc2 == 0)
        expected_after--;
      int urc3 = kur_loader_unload(m1);
      LOG_INFO("[TEST-KUR2] unload(hello) = %d (esperado 0)", urc3);
      if (urc3 == 0)
        expected_after--;
    }
    LOG_INFO("[TEST-KUR2] modulos tras test: %d (esperado %d)",
             kur_loader_count(), expected_after);
  }
}

// ===========================================================================
// system_init_task
//
// PID 0 real: arranca como primer task de kernel, coordina los workers y
// termina lanzando /usr/bin/init. Solo aquí usamos klog_boot_step/ok
// (secuencial, sin race con workers).
// ===========================================================================
static void system_init_task(void) {
  LOG_INFO("[KERNEL] system_init_task: inicio (id=%u)", sched_current()->id);

  // 1. Inicializar completions (una sola vez, antes de nada).
  completion_init(&g_acpi_done);
  completion_init(&g_fb_done);
  completion_init(&g_vfs_done);
  completion_init(&g_drivers_done);
  completion_init(&g_smp_done);
  completion_init(&g_timer_done);

  // 2. Timer IRQ (PIT) + abrir IRQs.
  extern void time_tick(void);
  irq_install_handler(0, time_tick);
  __asm__ volatile("sti");

  // 3. Timer definitivo (TSC + LAPIC). Inline, ~70 ms.
  timer_init_task(&g_timer_done);

  // 4. Workqueues + IRQ threaded. Necesitan scheduler + timer.
  klog_boot_step("[INIT] Workqueues globales...");
  if (workqueue_init() != 0)
    LOG_WARN("[INIT] workqueue_init() falló");
  else
    klog_boot_ok();

  klog_boot_step("[INIT] IRQ threaded...");
  if (irq_threaded_init() != 0)
    LOG_WARN("[INIT] irq_threaded_init() falló");
  else
    klog_boot_ok();

  // 5. Servicios permanentes del kernel.
  sched_create_task(ipc_echo_service);

  // 6. Spawn workers en paralelo.
  klog_boot_step("[INIT] Arrancando subsistemas en paralelo...");
  kthread_run(acpi_init_task, &g_acpi_done, "acpi-init");
  kthread_run(fb_init_task, &g_fb_done, "fb-init");
  kthread_run(vfs_init_task, &g_vfs_done, "vfs-init");
  kthread_run(drivers_init_task, &g_drivers_done, "drivers-init");
  kthread_run(net_init_task, NULL, "net-init");
  kthread_run(smp_boot_task, &g_smp_done, "smp-boot");
  klog_boot_ok();

  // 7. FB cuanto antes (para el splash).
  wait_for_completion_timeout(&g_fb_done, 3000);

  // 8. Subsistemas críticos.
  if (wait_for_completion_timeout(&g_vfs_done, 10000) == 0)
    LOG_WARN("[INIT] VFS timeout");
  if (wait_for_completion_timeout(&g_drivers_done, 15000) == 0)
    LOG_WARN("[INIT] Drivers timeout");
  if (wait_for_completion_timeout(&g_smp_done, 5000) == 0)
    LOG_WARN("[INIT] SMP timeout");
  else
    LOG_INFO("[INIT] SMP listo (%d CPUs online)", smp_online_cpus());

  // 9. Particiones + mounts (necesitan drivers).
  // [FASE 5.2] Con AHCI en Ring 3 los discos aparecen cuando ahci.aur
  // arranca y los registra; esperarlos antes de escanear particiones.
  // Solo si hay un HBA AHCI: sin él ahci.aur no se lanza y esperaría en vano.
  pci_device_t ahci_probe_dev;
  if (ahci_user_mode() && pci_find_device(0x01, 0x06, &ahci_probe_dev) == 0) {
    klog_boot_step("[INIT] Esperando ahci.aur...");
    if (blkp_wait_ready(5000))
      klog_boot_ok();
    else
      klog_boot_warn("[INIT] ahci.aur no registró discos (timeout)");
  }

  klog_boot_step("[INIT] Particiones...");
  part_scan_all();
  blkp_boot_scanned();
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
  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || b->is_partition || b->sector_size != 2048)
      continue;
    if (iso9660_mount_bdev(b->name, "/cdrom") == 0) {
      LOG_INFO("[INIT] Montado %s en /cdrom", b->name);
      break;
    }
  }

  // 10. Reveal de splash + compositor.
  // Sin framebuffer del firmware (GOP solo-Blt: virtio-gpu sin VGA...) se
  // espera a que el driver GPU .aur pida uno (GPUP_ALLOC_FB).
  if (!fb_ptr && gpup_display_expected()) {
    klog_boot_step("[INIT] Esperando driver de pantalla...");
    if (gpup_wait_display(10000))
      klog_boot_ok();
    else
      klog_boot_warn("[INIT] sin framebuffer ni driver de pantalla");
  }
  if (fb_ptr) {
    if (g_boot_splash_enabled && boot_splash_active()) {
      compositor_init();
      boot_splash_begin_reveal(NULL);
      while (boot_splash_tick_reveal())
        __asm__ volatile("pause");
      boot_splash_finish();
    } else {
      extern void boot_console_overlay_enable(int);
      boot_console_hook_remove();
      /* El overlay de logs del kernel arranca oculto; F12 lo muestra. */
      boot_console_overlay_enable(0);
      compositor_init();
    }
    sched_create_task(compositor_thread);
    LOG_INFO("[KERNEL] Compositor inicializado");
  }

  // 11. Tests en kthread paralelo. No bloquea: /usr/bin/init arranca
  // inmediatamente y los tests se ejecutan en background. El kthread
  // de tests arranca DHCP/ping al final (e1000e asume netif->ip == 0
  // durante los tests).
  task_t *test_task = sched_create_task(tests_thread);
  if (!test_task)
    LOG_WARN("[TEST] no se pudo crear tests_thread; tests NO ejecutados");

  // 12. Espacio de usuario (sin esperar a tests).
  klog_boot_step("[INIT] Cargando /usr/bin/init...");
  process_t *init_proc = process_load("/usr/bin/init");
  if (!init_proc) {
    klog_boot_warn("[INIT] /usr/bin/init no encontrado, fallback terminal");
    process_load("/usr/bin/terminal");
  } else {
    klog_boot_ok();
  }

  // 13. Salir.
  LOG_INFO("[KERNEL] system_init_task completado");
  task_t *cur = sched_current();
  if (cur)
    cur->state = TASK_DEAD;
  sched_yield();
  while (1)
    __asm__ volatile("sti; hlt");
}

// ===========================================================================
// kmain (sin cambios respecto al refactor anterior)
// ===========================================================================
void kmain(struct kernel_boot_info *kinfo) {
  // 1. IA32_GS_BASE defensivo.
  {
    extern cpu_local_t cpu_local_data[];
    uint64_t gs_base = (uint64_t)&cpu_local_data[0];
    uint32_t lo = (uint32_t)(gs_base & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(gs_base >> 32);
    __asm__ volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(0xC0000101));
    __asm__ volatile("wrmsr" ::"a"(0), "d"(0), "c"(0xC0000102));
  }

  // 2. Consola mínima.
  serial_init();
  klog_init();
  LOG_INFO("========================================");
  LOG_INFO("   AURORA OS KERNEL x86_64");
  LOG_INFO("========================================");

  // 3. SIMD + FPU.
  klog_boot_step("[BOOT] SSE/SIMD...");
  sse_init();
  cpu_simd_init();
  simd_init();
  klog_boot_ok();

  // 4. Boot info.
  memcpy(&boot, kinfo, sizeof(boot));
  g_boot_info = boot;

  // 5. Tablas de sistema.
  klog_boot_step("[BOOT] GDT/IDT/SYSCALL...");
  gdt_init();
  idt_init();
  syscall_init();
  cpu_local_data[0].kernel_stack =
      (uint64_t)(syscall_kernel_stack + sizeof(syscall_kernel_stack));
  cpu_local_data[0].user_rsp = 0;
  klog_boot_ok();

  // [FASE 3.3] Registrar el bus PCI antes de cualquier init paralelo.
  extern struct bus_type pci_bus;
  bus_register(&pci_bus);

  // [FASE 2.1c] Registrar el bus USB + drivers usbhid/storage/hub.
  // Debe ir antes de driver_register(&xhci_driver) para que cuando el
  // hotplug thread de xhci enumere el primer device USB, el bus ya
  // exista y tenga drivers que lo reclamen.
  extern int usb_bus_init(void);
  int usb_rc = usb_bus_init();
  if (usb_rc != 0)
    LOG_WARN("[BOOT] usb_bus_init falló: %d", usb_rc);

  // [FASE 3.1] Tabla de símbolos del kernel para .kur.
  ksymtab_init();

  // 6. Memoria.
  klog_boot_step("[BOOT] Memoria...");
  uint64_t max_phys_addr = 0;
  if (boot.memmap && boot.memmap_size > 0) {
    uint8_t *ptr = (uint8_t *)boot.memmap;
    for (uint64_t i = 0; i < boot.memmap_size; i += boot.memmap_desc_size) {
      uint32_t type = *(uint32_t *)(ptr + i + 0);
      if (type != EFI_CONVENTIONAL_MEMORY)
        continue;
      uint64_t phys = *(uint64_t *)(ptr + i + 8);
      uint64_t pages = *(uint64_t *)(ptr + i + 24);
      if (pages > UINT64_MAX / PAGE_SIZE)
        continue;
      uint64_t end = phys + pages * PAGE_SIZE;
      if (end < phys)
        continue;
      if (end > max_phys_addr)
        max_phys_addr = end;
    }
  }
  parse_memmap();

  pmm_init(boot.memmap, boot.memmap_size, boot.memmap_desc_size);
  extern void pmm_reserve_range(uint64_t, uint64_t);
  pmm_reserve_range(0x6000, 0x9000);

  extern void pmm_self_test(void);
  pmm_self_test();

  uint64_t cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  paging_init((uint64_t *)cr3, max_phys_addr);

  {
    uint64_t *pml4 = paging_get_pml4();
    uint64_t vstart = (uint64_t)&__text_start;
    uint64_t vend = (uint64_t)&__bss_end;
    uint64_t run_start = 0, run_end = 0;
    for (uint64_t v = vstart; v < vend; v += PAGE_SIZE) {
      uint64_t p = paging_get_phys_in(pml4, v);
      if (!p) {
        if (run_start)
          pmm_reserve_range(run_start, run_end);
        run_start = 0;
        continue;
      }
      if (run_start && p == run_end)
        run_end += PAGE_SIZE;
      else {
        if (run_start)
          pmm_reserve_range(run_start, run_end);
        run_start = p;
        run_end = p + PAGE_SIZE;
      }
    }
    if (run_start)
      pmm_reserve_range(run_start, run_end);
  }

  pmm_relocate_bitmap();
  uaccess_init();
  heap_init();
  klog_boot_ok();

  // [FASE 3.1] Loader de .kur (necesita heap + vmm_alloc_pages).
  kur_loader_init();

  // 7. ACPI MADT.
  klog_boot_step("[BOOT] ACPI MADT...");
  acpi_init(boot.acpi_rsdp);
  klog_boot_ok();

  // 8. APIC + PIT.
  klog_boot_step("[BOOT] LAPIC + IOAPIC + PIT...");
  apic_init();
  pit_init();
  klog_boot_ok();

  // 9. PF + swap.
  klog_boot_step("[BOOT] PF + swap...");
  pf_init();
  swap_init();
  klog_boot_ok();

  // 10. Per-CPU + IPI.
  klog_boot_step("[BOOT] SMP init + IPI...");
  smp_init();
  smp_set_bsp_lapic_id(lapic_get_bsp_id());
  ipi_init();
  klog_boot_ok();

  // 11. Scheduler + process.
  klog_boot_step("[BOOT] Scheduler + procesador...");
  sched_init();
  process_init();
  klog_boot_ok();

  // 12. GO.
  klog_boot_step("[BOOT] Saltando a multitarea...");
  task_t *t = sched_create_task(system_init_task);
  if (!t)
    LOG_PANIC("[KERNEL] no se pudo crear system_init_task");
  t->cpu_affinity = 0;
  sched_start(t);
  __builtin_unreachable();
}