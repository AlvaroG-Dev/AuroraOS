// kernel/smp_boot.c
volatile int smp_sched_active = 0;
#include "smp_boot.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "klog.h"
#include "paging.h"
#include "panic.h"
#include "pmm.h"
#include "sched.h"
#include "serial.h"
#include "string.h"

// Símbolos del trampoline (renombrados en el Makefile vía objcopy).
extern uint8_t smp_trampoline_start[];
extern uint8_t smp_trampoline_end[];
extern uint8_t smp_trampoline_size[];

extern uint64_t klog_get_tsc_freq(void);

smp_boot_params_t *smp_boot_params = NULL;
static int g_aps_booted = 0;

static volatile acpi_wakeup_mailbox_t *g_mailbox = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void delay_ms(int ms) {
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0) {
    for (volatile int i = 0; i < ms * 100000; i++)
      __asm__ volatile("pause");
    return;
  }
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t start = ((uint64_t)hi << 32) | lo;
  uint64_t target = (freq / 1000) * (uint64_t)ms;
  while (1) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now = ((uint64_t)hi << 32) | lo;
    if (now - start >= target)
      break;
    __asm__ volatile("pause");
  }
}

// [DIAG] Emitir un byte al puerto 0xE9 (QEMU debug console).
// Usar macro inline para evitar call/ret (más robusto en el AP).
#define DIAG(c) __asm__ volatile("outb %0, $0xE9" : : "a"((char)(c)))

// [DIAG] Dump del estado de smp_boot_params.
static void dump_smp_boot_params(const char *tag) {
  LOG_DEBUG("[%s] smp_boot_params = %p", tag, (void *)smp_boot_params);
  LOG_DEBUG("[%s]   ap_stack_top[0..1] = %llx, %llx", tag,
            (unsigned long long)smp_boot_params->ap_stack_top[0],
            (unsigned long long)smp_boot_params->ap_stack_top[1]);
  LOG_DEBUG("[%s]   aps_started = %d", tag, smp_boot_params->aps_started);
  LOG_DEBUG("[%s]   aps_ready = %d", tag, smp_boot_params->aps_ready);
  LOG_DEBUG("[%s]   bsp_apic_id = %u", tag, smp_boot_params->bsp_apic_id);
  LOG_DEBUG("[%s]   next_ap_apic_id = %u", tag,
            smp_boot_params->next_ap_apic_id);
}

// ---------------------------------------------------------------------------
// ACPI Multiprocessor Wakeup Mailbox
// ---------------------------------------------------------------------------
__attribute__((unused)) static int
acpi_mailbox_wakeup_ap(uint32_t apic_id, uint64_t wakeup_virt) {
  if (!g_mailbox)
    return -1;

  g_mailbox->apic_id = apic_id;
  g_mailbox->wakeup_vector = wakeup_virt;
  __sync_synchronize();

  g_mailbox->command = ACPI_MP_WAKE_COMMAND_WAKEUP;

  uint64_t freq = klog_get_tsc_freq();
  uint64_t target = (freq ? freq : 2000000000ULL) * 2;
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t start = ((uint64_t)hi << 32) | lo;

  while (g_mailbox->command != ACPI_MP_WAKE_COMMAND_IDLE) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now = ((uint64_t)hi << 32) | lo;
    if (now - start >= target) {
      LOG_WARN("[SMP] Mailbox timeout para apic_id=%u (command=%u)", apic_id,
               g_mailbox->command);
      return -1;
    }
    __asm__ volatile("pause");
  }

  return 0;
}

// ---------------------------------------------------------------------------
// INIT-SIPI-SIPI
// ---------------------------------------------------------------------------
static void lapic_send_ipi_raw(uint32_t apic_id, uint32_t icr_low) {
  lapic_write(LAPIC_REG_ICR_HIGH, (apic_id & 0xFF) << 24);
  lapic_write(LAPIC_REG_ICR_LOW, icr_low);
}

void smp_boot_init(void) {
  smp_boot_params = (smp_boot_params_t *)phys_to_virt(SMP_BOOT_PARAMS_PHYS);
  memset(smp_boot_params, 0, sizeof(*smp_boot_params));

  size_t tramp_size = (size_t)(smp_trampoline_end - smp_trampoline_start);
  if (tramp_size > PAGE_SIZE) {
    panic_noctx("smp_boot: trampoline demasiado grande (%zu bytes)",
                tramp_size);
  }

  void *dst = phys_to_virt(SMP_TRAMPOLINE_PHYS);
  memcpy(dst, smp_trampoline_start, tramp_size);

  LOG_DEBUG("[SMP] Trampoline copiado a 0x%llx (%zu bytes)",
            (unsigned long long)SMP_TRAMPOLINE_PHYS, tramp_size);

  // -------------------------------------------------------------------------
  // Calcular los valores a parchear.
  // -------------------------------------------------------------------------
  uint64_t pml4_virt = (uint64_t)paging_get_pml4();
  uint64_t pml4_phys = virt_to_phys((void *)pml4_virt);
  uint64_t entry_virt = (uint64_t)&ap_entry;

  if (pml4_phys > 0xFFFFFFFFULL) {
    panic_noctx("smp_boot: PML4 físico >4GB, el trampoline no lo soporta");
  }

  // -------------------------------------------------------------------------
  // Parchear el header del trampoline.
  // -------------------------------------------------------------------------
  smp_trampoline_header_t *hdr = (smp_trampoline_header_t *)dst;
  hdr->pml4_phys = pml4_phys;
  hdr->entry_virt = entry_virt;
  hdr->stack_top = 0; /* se escribe por AP en smp_boot_aps() */
  hdr->ap_index = 0;  /* se escribe por AP en smp_boot_aps() */

  LOG_DEBUG("[SMP] Trampoline configurado: pml4=0x%llx entry=0x%llx",
            (unsigned long long)pml4_phys, (unsigned long long)entry_virt);

  // -------------------------------------------------------------------------
  // Mailbox ACPI.
  // -------------------------------------------------------------------------
  const acpi_info_t *acpi = acpi_get_info();
  if (acpi->wakeup.present && acpi->wakeup.mailbox_paddr != 0) {
    g_mailbox = (volatile acpi_wakeup_mailbox_t *)phys_to_virt(
        acpi->wakeup.mailbox_paddr);
    LOG_DEBUG("[SMP] Mailbox ACPI en phys=0x%llx virt=%p (version=%u)",
              (unsigned long long)acpi->wakeup.mailbox_paddr, (void *)g_mailbox,
              acpi->wakeup.version);
  } else {
    g_mailbox = NULL;
    LOG_DEBUG("[SMP] Mailbox ACPI no disponible; se usará INIT-SIPI-SIPI");
  }
}

// ---------------------------------------------------------------------------
// ap_entry
//
// El trampoline nos deja en r12 el índice ACPI del AP (ap_index), leído
// del header en offset 0x20. Es lo PRIMERO que leemos aquí, antes de
// cualquier call C, para no depender de que r12 se preserve.
//
// Antes usábamos smp_boot_params->next_ap_apic_id y buscábamos el APIC
// ID en acpi->cpus[]. Eso tenía dos problemas:
//   1) Carrera: el BSP podía sobrescribir next_ap_apic_id antes de que
//      este AP lo leyera (ventana pequeña pero real).
//   2) ap_stack_top[] se indexaba con APIC ID en el BSP y con índice
//      ACPI aquí. Coincidían solo si APIC ID == índice ACPI (QEMU),
//      pero no en hardware real con APIC IDs no contiguos.
// ---------------------------------------------------------------------------
void ap_entry(void) {
  // -----------------------------------------------------------------------
  // PASO -1: leer el índice ACPI que el trampoline dejó en r12.
  // -----------------------------------------------------------------------
  uint64_t ap_index_64;
  __asm__ volatile("mov %%r12, %0" : "=r"(ap_index_64));
  uint32_t my_cpu = (uint32_t)ap_index_64;

  // ---------------------------------------------------------------------------
  // PASO 0: Activar SSE y FPU, y limpiar CD/NW del CR0.
  //
  // El trampoline ya limpia CD/NW antes de activar PG (ver
  // smp_trampoline.asm), pero lo repetimos aquí como defensa en
  // profundidad: si alguien modifica el trampoline y olvida ese paso,
  // los APs arrancarían con cache deshabilitada sin que se note en
  // KVM (donde el hardware lo ignora) pero ralentizando ~50x en TCG.
  // ---------------------------------------------------------------------------
  {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ULL << 2) | (1ULL << 3));   // clear EM, TS
    cr0 &= ~((1ULL << 29) | (1ULL << 30)); // clear NW, CD  [FIX]
    cr0 |= (1ULL << 1);                    // set MP
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("fninit");
  }

  DIAG('A');

  const acpi_info_t *acpi = acpi_get_info();
  DIAG('B');

  // Validar el índice contra ACPI y contra MAX_CPUS. Si el firmware
  // enumeró mal, o el trampoline nos pasó un índice inválido, mejor
  // caer aquí que corromper cpu_local_data de otra CPU.
  if (my_cpu >= (uint32_t)acpi->cpu_count || my_cpu >= MAX_CPUS) {
    DIAG('X');
    LOG_ERR("[AP] Índice ACPI inválido: %u (cpu_count=%d, MAX_CPUS=%d)", my_cpu,
            acpi->cpu_count, MAX_CPUS);
    while (1)
      __asm__ volatile("cli; hlt");
  }

  // Reconstruimos my_apic_id desde ACPI (solo para logs y para
  // cpu_local_data[my_cpu].lapic_id).
  uint32_t my_apic_id = acpi->cpus[my_cpu].apic_id;
  DIAG('C');

  // GDT y IDT para este AP
  gdt_ap_init(my_cpu);
  DIAG('F');

  extern void syscall_init_ap(void);
  syscall_init_ap();
  DIAG('S');

  /* [SIMD] Habilitar OSXSAVE + XCR0 en ESTE core. Es per-core, no basta
   * con lo que hace el BSP. Sin esto, el primer _mm256_* dará #UD. */
  extern void cpu_simd_init(void);
  cpu_simd_init();
  DIAG('s'); /* s minúscula para diferenciar del 'S' anterior */

  uint64_t star = rdmsr(MSR_STAR);
  LOG_DEBUG("[AP] CPU %d STAR=0x%lx (R3_CS=0x%lx)", my_cpu, (unsigned long)star,
            (unsigned long)((star >> 48) & 0xFFFF));

  idt_load();
  DIAG('G');

  // Configurar %gs apuntando a cpu_local_data[my_cpu]
  uint64_t cpu_ptr = (uint64_t)&cpu_local_data[my_cpu];
  wrmsr(MSR_GS_BASE, cpu_ptr);
  wrmsr(MSR_KERNEL_GS_BASE, 0);
  DIAG('H');

  cpu_local_data[my_cpu].cpu_id = my_cpu;
  cpu_local_data[my_cpu].lapic_id = my_apic_id;
  cpu_local_data[my_cpu].kernel_stack = smp_boot_params->ap_stack_top[my_cpu];
  cpu_local_data[my_cpu].current_task = NULL;
  cpu_local_data[my_cpu].ticks_since_resched = 0;
  DIAG('I');

  // Inicializar LAPIC local y su timer
  apic_init_ap();
  extern void lapic_timer_init_ap(void);
  lapic_timer_init_ap();
  DIAG('J');

  __sync_fetch_and_add(&smp_boot_params->aps_started, 1);
  __sync_fetch_and_add(&smp_boot_params->aps_ready, 1);
  DIAG('K');

  LOG_INFO("[AP] CPU %d (APIC ID %u) inicializado y listo para scheduler",
           my_cpu, my_apic_id);
  DIAG('L');

  extern volatile int smp_sched_active;
  while (!smp_sched_active) {
    __asm__ volatile("pause");
  }

  sched_start_ap();
}

// ---------------------------------------------------------------------------
// smp_boot_aps
// ---------------------------------------------------------------------------
int smp_aps_ready(void) {
  return smp_boot_params ? smp_boot_params->aps_ready : 0;
}

void smp_boot_aps(void) {
  if (g_aps_booted) {
    LOG_WARN("[SMP] smp_boot_aps ya ejecutado, ignorando");
    return;
  }
  g_aps_booted = 1;

  const acpi_info_t *acpi = acpi_get_info();
  if (!acpi->valid) {
    LOG_ERR("[SMP] ACPI no válido, no se pueden arrancar APs");
    return;
  }

  uint32_t bsp_id = lapic_get_bsp_id();
  smp_boot_params->bsp_apic_id = bsp_id;

  int expected = 0;
  for (int i = 0; i < acpi->cpu_count; i++) {
    if (!acpi->cpus[i].enabled)
      continue;
    if (acpi->cpus[i].apic_id == bsp_id)
      continue;
    expected++;
  }

  if (expected == 0) {
    LOG_INFO("[SMP] Sistema uniprocesador (no hay APs secundarios)");
    return;
  }

  LOG_INFO("[SMP] Arrancando %d procesador(es) de aplicación (AP)...",
           expected);
  dump_smp_boot_params("BSP-before");

  static uint8_t ap_stacks[MAX_CPUS][16 * 1024] __attribute__((aligned(16)));

  for (int i = 0; i < acpi->cpu_count; i++) {
    const acpi_cpu_t *c = &acpi->cpus[i];
    if (!c->enabled || c->apic_id == bsp_id)
      continue;
    // El índice en ap_stacks y ap_stack_top es el índice ACPI (i),
    // no el APIC ID. Así BSP y AP usan el mismo espacio de índices,
    // independientemente de si los APIC IDs son contiguos.
    if (i >= MAX_CPUS)
      continue;

    uint64_t stack_top = (uint64_t)&ap_stacks[i][0] + sizeof(ap_stacks[i]);
    smp_boot_params->ap_stack_top[i] = stack_top;

    /* Escribir stack_top y ap_index en el header del trampoline
     * (offsets 0x18 y 0x20) antes del SIPI. El AP los lee con
     * mov rsp,[0x7018] y mov r12,[0x7020]. */
    smp_trampoline_header_t *hdr =
        (smp_trampoline_header_t *)phys_to_virt(SMP_TRAMPOLINE_PHYS);
    hdr->stack_top = stack_top;
    hdr->ap_index = (uint64_t)i;
    __sync_synchronize();

    LOG_DEBUG("[SMP] Arrancando AP apic_id=%u idx=%d (stack_top=0x%016llx)",
              c->apic_id, i, (unsigned long long)stack_top);
    smp_boot_params->next_ap_apic_id = c->apic_id;
    __sync_synchronize();
    dump_smp_boot_params("BSP-after-set-next");

    int before = smp_boot_params->aps_ready;

    // INIT: delivery mode 5, level assert, edge trigger -> 0x00004500.
    // El original 0x00008500 tenía bit 15 (level trigger) a 1, que es
    // incorrecto para un INIT. En QEMU colaba; en hardware real, no.
    lapic_send_ipi_raw(c->apic_id, 0x00004500);
    // El segundo INIT del par INIT-SIPI-SIPI original tenía el mismo
    // problema; lo eliminamos y dejamos un solo INIT seguido de dos
    // SIPIs, que es lo que recomienda el SDM.
    delay_ms(10);

    DIAG('P');
    LOG_DEBUG("[SMP] SIPI enviado -> apic_id=%u", c->apic_id);
    // SIPI: delivery mode 6, edge trigger -> 0x00000600 | vector.
    lapic_send_ipi_raw(c->apic_id, 0x00000600 | SMP_TRAMPOLINE_VECTOR);
    delay_ms(1);
    lapic_send_ipi_raw(c->apic_id, 0x00000600 | SMP_TRAMPOLINE_VECTOR);
    DIAG('Q');

    for (int spin = 0; spin < 2000; spin++) {
      if (smp_boot_params->aps_ready > before)
        break;
      delay_ms(1);
    }
    dump_smp_boot_params("BSP-after-wait");

    if (smp_boot_params->aps_ready > before) {
      LOG_DEBUG("[SMP] AP apic_id=%u arrancado correctamente (aps_ready=%d)",
                c->apic_id, smp_boot_params->aps_ready);
    } else {
      LOG_ERR("[SMP] Timeout esperando al AP apic_id=%u (no respondió)",
              c->apic_id);
    }
  }

  if (smp_boot_params->aps_ready == expected) {
    LOG_INFO("[SMP] %d/%d APs listos y operativos", smp_boot_params->aps_ready,
             expected);
  } else {
    LOG_WARN("[SMP] Solo %d/%d APs respondieron (started=%d)",
             smp_boot_params->aps_ready, expected,
             smp_boot_params->aps_started);
  }
}
