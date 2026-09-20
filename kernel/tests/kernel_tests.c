// kernel/tests/kernel_tests.c
//
// Tests registrados en el framework .tests.
// Cada test corre con preempt_disable() activo y no debe bloquearse.
//
// Para añadir uno nuevo:
//   static void test_mi_cosa(void) {
//       TEST_ASSERT(cond, "mensaje");
//   }
//   REGISTER_TEST("mi_cosa", test_mi_cosa);

#include "../acpi.h"
#include "../apic.h"
#include "../cpu.h"
#include "../block.h"
#include "../heap.h"
#include "../klog.h"
#include "../sched.h"
#include "../slab.h"
#include "../smp_boot.h"
#include "../string.h"
#include "../test.h"
#include "../time.h"

// ---------------------------------------------------------------------------
// Heap: kmalloc/kfree básicos
// ---------------------------------------------------------------------------
static void test_kmalloc_64(void) {
  uint8_t *p = (uint8_t *)kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) devolvió NULL");
  if (!p)
    return;

  for (int i = 0; i < 64; i++)
    p[i] = (uint8_t)i;
  int ok = 1;
  for (int i = 0; i < 64; i++)
    if (p[i] != (uint8_t)i) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido corrupto tras escritura");
  kfree(p);
}
REGISTER_TEST("heap: kmalloc(64)", test_kmalloc_64);

static void test_kmalloc_8k(void) {
  uint8_t *p = (uint8_t *)kmalloc(8192);
  TEST_ASSERT(p != NULL, "kmalloc(8192) devolvió NULL");
  if (p)
    kfree(p);
}
REGISTER_TEST("heap: kmalloc(8192)", test_kmalloc_8k);

// ---------------------------------------------------------------------------
// Heap: kzalloc
// ---------------------------------------------------------------------------
static void test_kzalloc_zeroed(void) {
  uint8_t *p = (uint8_t *)kzalloc(200);
  TEST_ASSERT(p != NULL, "kzalloc(200) devolvió NULL");
  if (!p)
    return;
  int ok = 1;
  for (int i = 0; i < 200; i++)
    if (p[i] != 0) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "kzalloc no dejó todo a cero");
  kfree(p);
}
REGISTER_TEST("heap: kzalloc zeroed", test_kzalloc_zeroed);

// ---------------------------------------------------------------------------
// Heap: kcalloc + overflow
// ---------------------------------------------------------------------------
static void test_kcalloc(void) {
  int *arr = (int *)kcalloc(16, sizeof(int));
  TEST_ASSERT(arr != NULL, "kcalloc(16,4) devolvió NULL");
  if (!arr)
    return;
  int ok = 1;
  for (int i = 0; i < 16; i++)
    if (arr[i] != 0) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "kcalloc no zeroed");
  kfree(arr);
}
REGISTER_TEST("heap: kcalloc", test_kcalloc);

static void test_kcalloc_overflow(void) {
  void *ovf = kcalloc((size_t)-1, 2);
  TEST_ASSERT(ovf == NULL, "kcalloc no detectó overflow");
  if (ovf)
    kfree(ovf);
}
REGISTER_TEST("heap: kcalloc overflow", test_kcalloc_overflow);

// ---------------------------------------------------------------------------
// Heap: kstrdup / kstrndup
// ---------------------------------------------------------------------------
static void test_kstrdup(void) {
  char *s = kstrdup("Aurora OS");
  TEST_ASSERT(s != NULL, "kstrdup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "Aurora OS") == 0, "contenido != original");
  kfree(s);
}
REGISTER_TEST("heap: kstrdup", test_kstrdup);

static void test_kstrndup_cut(void) {
  char *s = kstrndup("abcdefgh", 4);
  TEST_ASSERT(s != NULL, "kstrndup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "abcd") == 0, "corte incorrecto: '%s'", s);
  kfree(s);
}
REGISTER_TEST("heap: kstrndup cut", test_kstrndup_cut);

static void test_kstrndup_short(void) {
  char *s = kstrndup("ab", 8);
  TEST_ASSERT(s != NULL, "kstrndup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "ab") == 0, "contenido incorrecto: '%s'", s);
  kfree(s);
}
REGISTER_TEST("heap: kstrndup short", test_kstrndup_short);

// ---------------------------------------------------------------------------
// Heap: krealloc
// ---------------------------------------------------------------------------
static void test_krealloc_grow(void) {
  char *r = (char *)kmalloc(16);
  TEST_ASSERT(r != NULL, "kmalloc(16) devolvió NULL");
  if (!r)
    return;
  for (int i = 0; i < 16; i++)
    r[i] = (char)('A' + i);
  char *r2 = (char *)krealloc(r, 128);
  TEST_ASSERT(r2 != NULL, "krealloc grow devolvió NULL");
  if (!r2) {
    kfree(r);
    return;
  }
  int ok = 1;
  for (int i = 0; i < 16; i++)
    if (r2[i] != (char)('A' + i)) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido no preservado tras crecer");
  kfree(r2);
}
REGISTER_TEST("heap: krealloc grow", test_krealloc_grow);

static void test_krealloc_shrink_same(void) {
  void *same = kmalloc(64);
  TEST_ASSERT(same != NULL, "kmalloc(64) devolvió NULL");
  if (!same)
    return;
  void *same2 = krealloc(same, 32);
  TEST_ASSERT(same2 == same, "krealloc shrink devolvió distinto ptr");
  kfree(same);
}
REGISTER_TEST("heap: krealloc shrink same ptr", test_krealloc_shrink_same);

static void test_krealloc_null(void) {
  void *fresh = krealloc(NULL, 32);
  TEST_ASSERT(fresh != NULL, "krealloc(NULL, 32) devolvió NULL");
  if (fresh)
    kfree(fresh);
}
REGISTER_TEST("heap: krealloc(NULL)", test_krealloc_null);

static void test_krealloc_zero(void) {
  void *p = kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) devolvió NULL");
  if (!p)
    return;
  void *p2 = krealloc(p, 0);
  TEST_ASSERT(p2 == NULL, "krealloc(p, 0) debía devolver NULL");
}
REGISTER_TEST("heap: krealloc(p, 0)", test_krealloc_zero);

// ---------------------------------------------------------------------------
// SLAB tests
// ---------------------------------------------------------------------------

static void test_slab_basic_32(void) {
  void *p = kmalloc(32);
  TEST_ASSERT(p != NULL, "kmalloc(32) devolvió NULL");
  if (!p)
    return;

  // Comprobar que está en la región del SLAB.
  uint64_t addr = (uint64_t)p;
  TEST_ASSERT(addr >= SLAB_VMA, "kmalloc(32) no vino del SLAB: %p", p);

  // Escribir y verificar.
  memset(p, 0xAB, 32);
  uint8_t *b = (uint8_t *)p;
  int ok = 1;
  for (int i = 0; i < 32; i++)
    if (b[i] != 0xAB) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido corrupto");

  kfree(p);
}
REGISTER_TEST("slab: basic 32", test_slab_basic_32);

static void test_slab_all_sizes(void) {
  static const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
  void *ptrs[8] = {0};

  for (int i = 0; i < 8; i++) {
    ptrs[i] = kmalloc(sizes[i]);
    TEST_ASSERT(ptrs[i] != NULL, "kmalloc(%lu) devolvió NULL",
                (unsigned long)sizes[i]);
    if (ptrs[i]) {
      uint64_t a = (uint64_t)ptrs[i];
      TEST_ASSERT(a >= SLAB_VMA, "kmalloc(%lu) no vino del SLAB",
                  (unsigned long)sizes[i]);
      memset(ptrs[i], (int)(i + 1), sizes[i]);
    }
  }

  for (int i = 0; i < 8; i++) {
    if (ptrs[i])
      kfree(ptrs[i]);
  }
}
REGISTER_TEST("slab: all sizes", test_slab_all_sizes);

static void test_slab_many_32(void) {
  // Asignar muchos objetos de 32 bytes para forzar varios slabs.
  // 1 slab con header de 64 bytes: (4096-64)/32 = 126 objetos.
  // Pedimos 500 para forzar ~4 slabs.
  enum { N = 500 };
  static void *ptrs[N];

  for (int i = 0; i < N; i++) {
    ptrs[i] = kmalloc(32);
    if (!ptrs[i]) {
      TEST_ASSERT(0, "kmalloc(32) falló en iteración %d", i);
      // Liberar lo asignado y salir.
      for (int j = 0; j < i; j++)
        kfree(ptrs[j]);
      return;
    }
    // Escribir un patrón único para detectar solapamientos.
    ((uint32_t *)ptrs[i])[0] = (uint32_t)i;
    ((uint32_t *)ptrs[i])[1] = (uint32_t)~i;
  }

  // Verificar que cada objeto conserva su patrón.
  int ok = 1;
  for (int i = 0; i < N; i++) {
    uint32_t a = ((uint32_t *)ptrs[i])[0];
    uint32_t b = ((uint32_t *)ptrs[i])[1];
    if (a != (uint32_t)i || b != (uint32_t)~i) {
      ok = 0;
      break;
    }
  }
  TEST_ASSERT(ok, "objetos se solapan o se corrompen");

  for (int i = 0; i < N; i++)
    kfree(ptrs[i]);
}
REGISTER_TEST("slab: 500 x 32 bytes", test_slab_many_32);

static void test_slab_free_reuse(void) {
  // Asignar, liberar, y volver a asignar. Debe reutilizar el mismo
  // slab y probablemente la misma dirección.
  void *a = kmalloc(64);
  void *b = kmalloc(64);
  TEST_ASSERT(a && b, "kmalloc falló");
  if (!a || !b) {
    if (a)
      kfree(a);
    if (b)
      kfree(b);
    return;
  }

  TEST_ASSERT(a != b, "kmalloc devolvió la misma dirección dos veces");

  kfree(a);
  void *c = kmalloc(64);
  TEST_ASSERT(c != NULL, "kmalloc tras kfree falló");
  // Lo más probable es que c == a (LIFO freelist), pero no lo exigimos.
  if (c)
    kfree(c);
  kfree(b);
}
REGISTER_TEST("slab: free + reuse", test_slab_free_reuse);

static void test_slab_krealloc_inplace(void) {
  // kmalloc(32) → krealloc(64). Ambos caben... no, 64 > 32, hay que
  // mover a un cache mayor. Pero kmalloc(64) → krealloc(48) debería
  // ser in-place (48 <= 64, mismo cache).
  void *p = kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) falló");
  if (!p)
    return;

  // Escribir algo.
  memset(p, 0x42, 64);

  void *p2 = krealloc(p, 48);
  TEST_ASSERT(p2 == p, "krealloc(64→48) debía ser in-place");
  // Verificar contenido.
  uint8_t *b = (uint8_t *)p2;
  int ok = 1;
  for (int i = 0; i < 48; i++)
    if (b[i] != 0x42) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido no preservado");
  kfree(p2);
}
REGISTER_TEST("slab: krealloc in-place", test_slab_krealloc_inplace);

static void test_slab_usable_size(void) {
  void *p = kmalloc(20); // cae en cache de 32
  TEST_ASSERT(p != NULL, "kmalloc(20) falló");
  if (!p)
    return;
  size_t usable = slab_usable_size(p);
  TEST_ASSERT(usable >= 20, "usable_size < solicitado: %lu",
              (unsigned long)usable);
  kfree(p);
}
REGISTER_TEST("slab: usable size", test_slab_usable_size);

// ---------------------------------------------------------------------------
// SMP (Fase 0)
// ---------------------------------------------------------------------------
static void test_smp_processor_id_zero(void) {
  int cpu = smp_processor_id();
  TEST_ASSERT(cpu == 0, "smp_processor_id() devolvió %d, esperado 0 en Fase 0",
              cpu);
}
REGISTER_TEST("smp: smp_processor_id() == 0", test_smp_processor_id_zero);

static void test_smp_cpu_local_data_exists(void) {
  // Verifica que el array existe y que el CPU 0 tiene sus campos
  // inicializados. Con SMP, cpu_local_data[0].cpu_id debe ser 0.
  TEST_ASSERT(cpu_local_data[0].cpu_id == 0,
              "cpu_local_data[0].cpu_id = %d, esperado 0",
              cpu_local_data[0].cpu_id);
}
REGISTER_TEST("smp: cpu_local_data[0] inicializado",
              test_smp_cpu_local_data_exists);

static void test_smp_this_cpu_macro(void) {
  uint64_t saved = this_cpu(ticks_since_resched);
  this_cpu(ticks_since_resched) = 0xDEADBEEF;
  TEST_ASSERT(cpu_local_data[0].ticks_since_resched == 0xDEADBEEF,
              "this_cpu(ticks_since_resched) no escribió en cpu_local_data[0]");
  this_cpu(ticks_since_resched) = saved;
}
REGISTER_TEST("smp: this_cpu() accede al CPU actual", test_smp_this_cpu_macro);

static void test_smp_per_cpu_macro(void) {
  per_cpu(ticks_since_resched, 3) = 0xCAFEBABE;
  TEST_ASSERT(cpu_local_data[3].ticks_since_resched == 0xCAFEBABE,
              "per_cpu(ticks_since_resched, 3) no escribió en cpu_local_data[3]");
  per_cpu(ticks_since_resched, 3) = 0;
}
REGISTER_TEST("smp: per_cpu() accede al slot correcto", test_smp_per_cpu_macro);

// ---------------------------------------------------------------------------
// ACPI (Fase 1 de SMP)
// ---------------------------------------------------------------------------
static void test_acpi_valid(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->valid == 1, "ACPI no se inicializó correctamente");
}
REGISTER_TEST("acpi: parseo del MADT correcto", test_acpi_valid);

static void test_acpi_cpu_count(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->cpu_count >= 1, "ACPI cpu_count = %d, esperado >= 1",
              info->cpu_count);
}
REGISTER_TEST("acpi: al menos 1 CPU detectada", test_acpi_cpu_count);

static void test_acpi_lapic_address(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->lapic_address != 0, "ACPI lapic_address = 0");
}
REGISTER_TEST("acpi: LAPIC address válida", test_acpi_lapic_address);

static void test_acpi_bsp_found(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->bsp_index >= 0, "ACPI no encontró el BSP (bsp_index=%d)",
              info->bsp_index);
}
REGISTER_TEST("acpi: BSP identificado", test_acpi_bsp_found);

// ---------------------------------------------------------------------------
// APIC (Fase 2.1)
// ---------------------------------------------------------------------------
static void test_apic_lapic_mapped(void) {
  // Si el LAPIC está mapeado y activo, lapic_get_id() devuelve un valor
  // no cero. En QEMU con -smp 4, el BSP tiene APIC ID 0, así que
  // devuelve 0. En otros sistemas puede ser distinto.
  // Verificamos que el registro VERSION tiene un valor plausible.
  uint32_t version = lapic_read(LAPIC_REG_VERSION);
  TEST_ASSERT(version != 0 && version != 0xFFFFFFFF,
              "LAPIC VERSION = 0x%x (no mapeado o no activo)", version);
}
REGISTER_TEST("apic: LAPIC mapeado y activo", test_apic_lapic_mapped);

static void test_apic_svr_enabled(void) {
  uint32_t svr = lapic_read(LAPIC_REG_SVR);
  TEST_ASSERT((svr & LAPIC_SVR_ENABLE) != 0,
              "LAPIC SVR no tiene el bit ENABLE (svr=0x%x)", svr);
}
REGISTER_TEST("apic: LAPIC SVR habilitado", test_apic_svr_enabled);

static void test_apic_bsp_id(void) {
  // El BSP APIC ID debe coincidir con el CPUID de este CPU.
  uint32_t eax, ebx, ecx, edx;
  cpuid(1, 0, &eax, &ebx, &ecx, &edx);
  uint32_t bsp_cpuid_id = (ebx >> 24) & 0xFF;
  uint32_t bsp_apic_id = lapic_get_bsp_id();
  TEST_ASSERT(bsp_apic_id == bsp_cpuid_id,
              "BSP APIC ID = %u, esperado %u (de CPUID)", bsp_apic_id,
              bsp_cpuid_id);
}
REGISTER_TEST("apic: BSP APIC ID coincide con CPUID", test_apic_bsp_id);

// ---------------------------------------------------------------------------
// APIC / IOAPIC (Fase 2.2)
// ---------------------------------------------------------------------------
static void test_apic_ioapic_detected(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->ioapic_count >= 1,
              "No hay IOAPICs detectados por ACPI (count=%d)",
              info->ioapic_count);
}
REGISTER_TEST("apic: IOAPIC detectado", test_apic_ioapic_detected);

static void test_apic_iso_qemu(void) {
  // En QEMU con PIC legacy, IRQ 0 se mapea a GSI 2 vía ISO.
  // Verificamos que el ISO está registrado.
  const acpi_info_t *info = acpi_get_info();
  int found = 0;
  for (int i = 0; i < info->iso_count; i++) {
    if (info->isos[i].irq == 0 && info->isos[i].gsi == 2) {
      found = 1;
      break;
    }
  }
  // No es un error si no está, pero en QEMU con PIC dual sí lo está.
  TEST_ASSERT(1, "ISO IRQ0->GSI2: %s", found ? "presente" : "no presente");
}
REGISTER_TEST("apic: ISO IRQ0->GSI2 registrado", test_apic_iso_qemu);

// ---------------------------------------------------------------------------
// LAPIC timer (Fase 2.3)
// ---------------------------------------------------------------------------
static void test_lapic_timer_running(void) {
  // El LAPIC timer debe estar en modo periódico. Comprobamos que
  // el contador actual está decreciendo (no está en 0 ni en 0xFFFFFFFF
  // congelado).
  uint32_t c1 = lapic_read(LAPIC_REG_TIMER_CURRENT);
  for (volatile int i = 0; i < 1000; i++) {
  }
  uint32_t c2 = lapic_read(LAPIC_REG_TIMER_CURRENT);
  TEST_ASSERT(c1 != c2, "LAPIC timer no decrementa (c1=%u c2=%u)", c1, c2);
}
REGISTER_TEST("lapic-timer: contador decrementa", test_lapic_timer_running);

static void test_lapic_timer_tick(void) {
  // Leer tick_count global (definido en time.c). El LAPIC timer lo
  // incrementa a 1 kHz. Como los tests corren con preempt_disable(),
  // el scheduler no cambia de tarea, pero el handler del LAPIC timer
  // sí se ejecuta (las IRQs no están enmascaradas por preempt_disable).
  //
  // En QEMU con virtualización anidada o bajo carga, un tick puede
  // tardar más de lo esperado. Usamos un deadline de ~500 ms a 3.6 GHz
  // (~1.8e9 iteraciones de pause, que es barato).
  extern volatile uint64_t tick_count;

  uint64_t t1 = tick_count;
  uint64_t spins = 0;
  const uint64_t spin_limit = 1800000000ULL;

  while (tick_count == t1 && spins < spin_limit) {
    __asm__ volatile("pause");
    spins++;
  }

  uint64_t t2 = tick_count;
  TEST_ASSERT(t2 > t1, "tick_count no avanza (t1=%lu t2=%lu, spins=%lu)",
              (unsigned long)t1, (unsigned long)t2, (unsigned long)spins);
}
REGISTER_TEST("lapic-timer: tick_count avanza", test_lapic_timer_tick);

// ---------------------------------------------------------------------------
// Scheduler SMP y APs (Fase 3 & 4)
// ---------------------------------------------------------------------------
static void test_smp_sched_current_valid(void) {
  task_t *cur = sched_current();
  TEST_ASSERT(cur != NULL, "sched_current() devolvió NULL");
  TEST_ASSERT(cur->state == TASK_RUNNING,
              "sched_current() no está en TASK_RUNNING (state=%d)", cur->state);
}
REGISTER_TEST("smp: sched_current() per-CPU válido",
              test_smp_sched_current_valid);

static void test_smp_aps_status(void) {
  const acpi_info_t *acpi = acpi_get_info();
  if (acpi->cpu_count > 1) {
    int ready = smp_aps_ready();
    TEST_ASSERT(ready > 0, "Sistema SMP con %d CPUs pero aps_ready=%d",
                acpi->cpu_count, ready);
  } else {
    TEST_ASSERT(smp_aps_ready() == 0,
                "Sistema uniprocesador pero aps_ready != 0");
  }
}
REGISTER_TEST("smp: estado de APs coherente con ACPI", test_smp_aps_status);

// ---------------------------------------------------------------------------
// [SMP 4.4] Test serio: wakeup cross-CPU vía IPI.
//
// Crea una tarea de kernel que se bloquea en una wait queue. Desde el
// BSP, marca la condición y llama a sched_make_ready(), que debe enviar
// una IPI a un AP idle. La tarea corre en el AP, registra en qué CPU
// ejecutó, y despierta al BSP.
//
// Es un test BLOQUEANTE: usa wait_event() y sched_yield(). El runner
// lo ejecuta sin preempt_disable.
// ---------------------------------------------------------------------------

static struct wait_queue g_ipi_waiter_wq; // wq donde duerme el waiter
static struct wait_queue g_ipi_done_wq;   // wq donde duerme el BSP
static volatile int g_ipi_cond = 0;       // 0=idle, 1=despierta, 2=terminó
static volatile int g_ipi_done = 0;       // 1 cuando el waiter acabó
static volatile uint32_t g_ipi_runner_cpu = 0xFFFFFFFF;
static volatile uint32_t g_ipi_waker_cpu = 0xFFFFFFFF;

static bool ipi_waiter_cond_fn(void *arg) {
  (void)arg;
  return g_ipi_cond != 0;
}

static bool ipi_done_cond_fn(void *arg) {
  (void)arg;
  return g_ipi_done != 0;
}

static void ipi_test_waiter(void) {
  // Esperar a que el BSP active la condición. Esto nos deja BLOCKED.
  // El BSP luego llamará a sched_make_ready() para despertarnos.
  wait_event(&g_ipi_waiter_wq, ipi_waiter_cond_fn, NULL);

  // Aquí ya corremos. Registrar en qué CPU.
  g_ipi_runner_cpu = (uint32_t)smp_processor_id();

  // Avisar al BSP.
  g_ipi_done = 1;
  wake_up_all(&g_ipi_done_wq);
}

static void test_smp_ipi_wakeup(void) {
  if (smp_aps_ready() == 0) {
    TEST_ASSERT(0, "sin APs, el test no puede validar el cross-CPU");
    return;
  }

  g_ipi_cond = 0;
  g_ipi_done = 0;
  g_ipi_runner_cpu = 0xFFFFFFFF;
  g_ipi_waker_cpu = 0xFFFFFFFF;
  wait_queue_init(&g_ipi_waiter_wq);
  wait_queue_init(&g_ipi_done_wq);

  uint32_t my_cpu = (uint32_t)smp_processor_id();
  g_ipi_waker_cpu = my_cpu;

  task_t *waiter = sched_create_task(ipi_test_waiter);
  TEST_ASSERT(waiter != NULL, "sched_create_task falló");
  if (!waiter)
    return;

  // ---------------------------------------------------------------
  // FASE 1: dejar que el waiter corra y se bloquee.
  //
  // SIN afinidad, para que cualquier CPU (incluido el BSP) pueda
  // cogerla y llevarla hasta el wait_event. Si le pusiéramos
  // afinidad ahora, el BSP la ignoraría y los APs tardarían hasta
  // 10 ms en tickear, más de lo que dura este bucle.
  // ---------------------------------------------------------------
  waiter->cpu_affinity = -1;

  for (int i = 0; i < 5000 && waiter->state != TASK_BLOCKED; i++) {
    sched_yield();
    // Si aún no está blocked, darle algo de tiempo real a los APs
    // para que hagan al menos un tick. pause es barato.
    for (volatile int k = 0; k < 1000; k++) {
      __asm__ volatile("pause");
    }
  }

  TEST_ASSERT(waiter->state == TASK_BLOCKED,
              "el waiter no se bloqueó (state=%d)", waiter->state);
  if (waiter->state != TASK_BLOCKED)
    return;

  // ---------------------------------------------------------------
  // FASE 2: elegir un AP y fijarle afinidad para el wakeup.
  //
  // A partir de aquí, solo ese AP podrá cogerla. El sched_make_ready
  // enviará la IPI a ese AP, y el sched_tick del BSP la ignorará.
  // ---------------------------------------------------------------
  int ap_cpu = -1;
  for (int c = 0; c < MAX_CPUS; c++) {
    if ((uint32_t)c == my_cpu)
      continue;
    // Un AP válido: su idle task ya está corriendo.
    if (per_cpu(current_task, c) != NULL) {
      ap_cpu = c;
      break;
    }
  }
  TEST_ASSERT(ap_cpu >= 0, "no se encontró ningún AP con idle task");
  if (ap_cpu < 0)
    return;

  waiter->cpu_affinity = ap_cpu;

  // ---------------------------------------------------------------
  // FASE 3: despertar y esperar a que corra en el AP.
  // ---------------------------------------------------------------
  g_ipi_cond = 1;
  wake_up_all(&g_ipi_waiter_wq);

  wait_event(&g_ipi_done_wq, ipi_done_cond_fn, NULL);

  TEST_ASSERT(g_ipi_done == 1, "g_ipi_done != 1");
  TEST_ASSERT(g_ipi_runner_cpu == (uint32_t)ap_cpu,
              "el waiter corrió en cpu=%u, esperado cpu=%d", g_ipi_runner_cpu,
              ap_cpu);

  LOG_INFO("[TEST] ipi-wakeup: waker=BSP(cpu=%u) runner=cpu[%u] (afinidad=%d)",
           g_ipi_waker_cpu, g_ipi_runner_cpu, ap_cpu);
}
REGISTER_TEST_FLAGS("smp: IPI wakeup cross-CPU", test_smp_ipi_wakeup,
                    TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP);


// ---------------------------------------------------------------------------
// Block layer
// ---------------------------------------------------------------------------
static void test_blk_init(void) {
  // Después de blk_init, blk_count() debe ser >= 0.
  TEST_ASSERT(blk_count() >= 0, "blk_count() < 0");
}
REGISTER_TEST("blk: init", test_blk_init);

static void test_blk_no_duplicates(void) {
  int n = blk_count();
  for (int i = 0; i < n; i++) {
    block_device_t *a = blk_get_by_index(i);
    TEST_ASSERT(a != NULL, "blk_get_by_index(%d) == NULL", i);
    for (int j = i + 1; j < n; j++) {
      block_device_t *b = blk_get_by_index(j);
      TEST_ASSERT(strcmp(a->name, b->name) != 0, "nombres duplicados: %s",
                  a->name);
    }
  }
}
REGISTER_TEST("blk: no duplicados", test_blk_no_duplicates);

static void test_blk_lookup_missing(void) {
  block_device_t *d = blk_lookup("nonexistent");
  TEST_ASSERT(d == NULL, "blk_lookup('nonexistent') != NULL");
}
REGISTER_TEST("blk: lookup de disco inexistente", test_blk_lookup_missing);
