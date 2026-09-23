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
#include "../ahci.h"
#include "../apic.h"
#include "../ata_pio.h"
#include "../atapi.h"
#include "../block.h"
#include "../cpu.h"
#include "../heap.h"
#include "../klog.h"
#include "../paging.h"
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

  uint64_t addr = (uint64_t)p;
  TEST_ASSERT(addr >= SLAB_VMA, "kmalloc(32) no vino del SLAB: %p", p);

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
  enum { N = 500 };
  static void *ptrs[N];

  for (int i = 0; i < N; i++) {
    ptrs[i] = kmalloc(32);
    if (!ptrs[i]) {
      TEST_ASSERT(0, "kmalloc(32) falló en iteración %d", i);
      for (int j = 0; j < i; j++)
        kfree(ptrs[j]);
      return;
    }
    ((uint32_t *)ptrs[i])[0] = (uint32_t)i;
    ((uint32_t *)ptrs[i])[1] = (uint32_t)~i;
  }

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
  if (c)
    kfree(c);
  kfree(b);
}
REGISTER_TEST("slab: free + reuse", test_slab_free_reuse);

static void test_slab_krealloc_inplace(void) {
  void *p = kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) falló");
  if (!p)
    return;

  memset(p, 0x42, 64);

  void *p2 = krealloc(p, 48);
  TEST_ASSERT(p2 == p, "krealloc(64→48) debía ser in-place");
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
  void *p = kmalloc(20);
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
static void test_smp_processor_id_valid(void) {
  int cpu = smp_processor_id();
  TEST_ASSERT(cpu >= 0 && cpu < MAX_CPUS,
              "smp_processor_id() devolvió %d, fuera de rango [0,%d)",
              cpu, MAX_CPUS);
  TEST_ASSERT(cpu_local_data[cpu].cpu_id == cpu,
              "cpu_local_data[%d].cpu_id = %d, esperado %d",
              cpu, cpu_local_data[cpu].cpu_id, cpu);
}
REGISTER_TEST("smp: smp_processor_id() válido", test_smp_processor_id_valid);

static void test_smp_cpu_local_data_exists(void) {
  TEST_ASSERT(cpu_local_data[0].cpu_id == 0,
              "cpu_local_data[0].cpu_id = %d, esperado 0",
              cpu_local_data[0].cpu_id);
}
REGISTER_TEST("smp: cpu_local_data[0] inicializado",
              test_smp_cpu_local_data_exists);

static void test_smp_this_cpu_macro(void) {
  int cpu = smp_processor_id();
  uint64_t saved = this_cpu(ticks_since_resched);
  this_cpu(ticks_since_resched) = 0xDEADBEEF;
  TEST_ASSERT(cpu_local_data[cpu].ticks_since_resched == 0xDEADBEEF,
              "this_cpu(ticks_since_resched) no escribió en cpu_local_data[%d]",
              cpu);
  this_cpu(ticks_since_resched) = saved;
}
REGISTER_TEST("smp: this_cpu() accede al CPU actual", test_smp_this_cpu_macro);

static void test_smp_per_cpu_macro(void) {
  per_cpu(ticks_since_resched, 3) = 0xCAFEBABE;
  TEST_ASSERT(
      cpu_local_data[3].ticks_since_resched == 0xCAFEBABE,
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
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->bsp_index >= 0 && info->bsp_index < info->cpu_count,
              "ACPI bsp_index=%d inválido (cpu_count=%d)",
              info->bsp_index, info->cpu_count);
  if (info->bsp_index < 0 || info->bsp_index >= info->cpu_count)
    return;

  uint32_t expected = info->cpus[info->bsp_index].apic_id;
  uint32_t actual = lapic_get_bsp_id();
  TEST_ASSERT(actual == expected,
              "BSP APIC ID = %u, esperado %u (MADT)", actual, expected);
}

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
  const acpi_info_t *info = acpi_get_info();
  int found = 0;
  for (int i = 0; i < info->iso_count; i++) {
    if (info->isos[i].irq == 0 && info->isos[i].gsi == 2) {
      found = 1;
      break;
    }
  }
  TEST_ASSERT(1, "ISO IRQ0->GSI2: %s", found ? "presente" : "no presente");
}
REGISTER_TEST("apic: ISO IRQ0->GSI2 registrado", test_apic_iso_qemu);

// ---------------------------------------------------------------------------
// LAPIC timer (Fase 2.3)
// ---------------------------------------------------------------------------
static void test_lapic_timer_running(void) {
  uint32_t c1 = lapic_read(LAPIC_REG_TIMER_CURRENT);
  for (volatile int i = 0; i < 1000; i++) {
  }
  uint32_t c2 = lapic_read(LAPIC_REG_TIMER_CURRENT);
  TEST_ASSERT(c1 != c2, "LAPIC timer no decrementa (c1=%u c2=%u)", c1, c2);
}
REGISTER_TEST("lapic-timer: contador decrementa", test_lapic_timer_running);

static void test_lapic_timer_tick(void) {
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
// ---------------------------------------------------------------------------
static struct wait_queue g_ipi_waiter_wq;
static struct wait_queue g_ipi_done_wq;
static volatile int g_ipi_cond = 0;
static volatile int g_ipi_done = 0;
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
  wait_event(&g_ipi_waiter_wq, ipi_waiter_cond_fn, NULL);
  g_ipi_runner_cpu = (uint32_t)smp_processor_id();
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

  waiter->cpu_affinity = -1;

  for (int i = 0; i < 5000 && waiter->state != TASK_BLOCKED; i++) {
    sched_yield();
    for (volatile int k = 0; k < 1000; k++) {
      __asm__ volatile("pause");
    }
  }

  TEST_ASSERT(waiter->state == TASK_BLOCKED,
              "el waiter no se bloqueó (state=%d)", waiter->state);
  if (waiter->state != TASK_BLOCKED)
    return;

  int ap_cpu = -1;
  for (int c = 0; c < MAX_CPUS; c++) {
    if ((uint32_t)c == my_cpu)
      continue;
    if (per_cpu(current_task, c) != NULL) {
      ap_cpu = c;
      break;
    }
  }
  TEST_ASSERT(ap_cpu >= 0, "no se encontró ningún AP con idle task");
  if (ap_cpu < 0)
    return;

  waiter->cpu_affinity = ap_cpu;

  g_ipi_cond = 1;
  wake_up_all(&g_ipi_waiter_wq);

  wait_event(&g_ipi_done_wq, ipi_done_cond_fn, NULL);

  TEST_ASSERT(g_ipi_done == 1, "g_ipi_done != 1");
  TEST_ASSERT(g_ipi_runner_cpu == (uint32_t)ap_cpu,
              "el waiter corrió en cpu=%u, esperado cpu=%d", g_ipi_runner_cpu,
              ap_cpu);

  LOG_INFO("[TEST] ipi-wakeup: waker=cpu[%u] runner=cpu[%u] (afinidad=%d)",
           g_ipi_waker_cpu, g_ipi_runner_cpu, ap_cpu);
}
REGISTER_TEST_FLAGS("smp: IPI wakeup cross-CPU", test_smp_ipi_wakeup,
                    TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP);

// ---------------------------------------------------------------------------
// Scheduler: timeout wakeup with >16 distinct wait queues
//
// Regression test for sched_wake_expired(): the old implementation kept a
// fixed array of 16 wait queues. With 17 expired tasks sleeping on 17
// different queues, the 17th queue was never awakened and its task could
// remain BLOCKED forever.
//
// The test deliberately puts all 17 tasks into distinct wait queues. Once
// every waiter is blocked, it advances the kernel tick counter past the
// common timeout while interrupts are disabled and calls sched_wake_expired()
// exactly once. This keeps the test deterministic while preserving the real
// wait_event_interruptible_timeout() deadline used by each waiter.
// ---------------------------------------------------------------------------
#define TIMEOUT_WQ_TEST_COUNT 17
#define TIMEOUT_WQ_TEST_TIMEOUT 100

typedef struct timeout_wq_test_slot {
  wait_queue_t wq;
  task_t *task;
  uint32_t task_id;
  volatile int completed;
  volatile long result;
} timeout_wq_test_slot_t;

static timeout_wq_test_slot_t g_timeout_wq_slots[TIMEOUT_WQ_TEST_COUNT];
static volatile int g_timeout_wq_completed = 0;

static bool timeout_wq_never_ready(void *arg) {
  (void)arg;
  return false;
}

static timeout_wq_test_slot_t *timeout_wq_find_slot(task_t *task) {
  if (!task)
    return NULL;

  for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
    if (g_timeout_wq_slots[i].task == task ||
        g_timeout_wq_slots[i].task_id == task->id)
      return &g_timeout_wq_slots[i];
  }

  return NULL;
}

static void timeout_wq_test_waiter(void) {
  task_t *self = sched_current();
  timeout_wq_test_slot_t *slot = NULL;

  // sched_create_task() publishes the task before returning it. If this
  // task gets scheduled before the test has stored its pointer/ID in the
  // slot, yield and let the creator finish the association.
  while ((slot = timeout_wq_find_slot(self)) == NULL)
    sched_yield();

  slot->result =
      wait_event_interruptible_timeout(&slot->wq, timeout_wq_never_ready,
                                       NULL, TIMEOUT_WQ_TEST_TIMEOUT);
  slot->completed = 1;
  __sync_fetch_and_add(&g_timeout_wq_completed, 1);
}

static void test_sched_timeout_more_than_16_queues(void) {
  g_timeout_wq_completed = 0;

  for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
    wait_queue_init(&g_timeout_wq_slots[i].wq);
    g_timeout_wq_slots[i].task = NULL;
    g_timeout_wq_slots[i].task_id = UINT32_MAX;
    g_timeout_wq_slots[i].completed = 0;
    g_timeout_wq_slots[i].result = -1;
  }

  // Create all waiters. They all use the same finite timeout. We will
  // advance tick_count after all 17 have reached TASK_BLOCKED, so no waiter
  // can expire naturally before the complete set is ready.
  for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
    task_t *task = sched_create_task(timeout_wq_test_waiter);
    if (!task) {
      TEST_ASSERT(0, "sched_create_task falló en waiter %d", i);
      // Avoid leaving already-created waiters permanently blocked.
      for (int j = 0; j < i; j++)
        wake_up_all(&g_timeout_wq_slots[j].wq);
      return;
    }

    g_timeout_wq_slots[i].task = task;
    g_timeout_wq_slots[i].task_id = task->id;
  }

  // Wait until every task is actually sleeping on its own distinct queue.
  uint64_t block_deadline = sched_get_ticks() + 2000;
  while (1) {
    int blocked = 0;
    for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
      task_t *task = g_timeout_wq_slots[i].task;
      if (task && task->state == TASK_BLOCKED &&
          task->waiting_on == &g_timeout_wq_slots[i].wq)
        blocked++;
    }

    if (blocked == TIMEOUT_WQ_TEST_COUNT)
      break;

    if (sched_get_ticks() >= block_deadline) {
      TEST_ASSERT(0, "solo %d/%d waiters llegaron a TASK_BLOCKED", blocked,
                  TIMEOUT_WQ_TEST_COUNT);
      for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++)
        wake_up_all(&g_timeout_wq_slots[i].wq);
      return;
    }

    sched_yield();
  }

  /*
   * Make all real wait_event() deadlines expire simultaneously.
   *
   * We deliberately advance tick_count rather than overwriting each task's
   * wake_deadline. wait_common() keeps its own local deadline, so changing
   * only task->wake_deadline would wake the task but make wait_common() think
   * its timeout had not elapsed; that was the flaw in the first version of
   * this regression test.
   *
   * Disabling interrupts prevents the normal timer handler from racing with
   * the single explicit sched_wake_expired() call below.
   */
  unsigned long flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");

  uint64_t now = tick_count;
  tick_count = now + TIMEOUT_WQ_TEST_TIMEOUT + 1;

  // With the old implementation this wakes only 16 distinct queues.
  sched_wake_expired();

  __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");

  // Every waiter must actually run after being made READY and observe a
  // timeout return (0), not remain blocked or re-enter the wait.
  uint64_t done_deadline = sched_get_ticks() + 2000;
  while (g_timeout_wq_completed < TIMEOUT_WQ_TEST_COUNT &&
         sched_get_ticks() < done_deadline) {
    sched_yield();
  }

  // Aggregate the result into one assertion so one broken scheduler path
  // produces one failing test rather than dozens of failures.
  int completed = g_timeout_wq_completed;
  int bad_results = 0;
  for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
    if (g_timeout_wq_slots[i].completed != 1 ||
        g_timeout_wq_slots[i].result != 0)
      bad_results++;
  }

  TEST_ASSERT(completed == TIMEOUT_WQ_TEST_COUNT && bad_results == 0,
              "timeout wakeup incorrect: completed=%d/%d, resultados_invalidos=%d",
              completed, TIMEOUT_WQ_TEST_COUNT, bad_results);

  if (completed == TIMEOUT_WQ_TEST_COUNT && bad_results == 0) {
    LOG_INFO("[TEST] timeout-wq: %d/%d waiters despertados y retornaron timeout",
             TIMEOUT_WQ_TEST_COUNT, TIMEOUT_WQ_TEST_COUNT);
  }
}
REGISTER_TEST_FLAGS("sched: timeout wakeup >16 wait queues",
                    test_sched_timeout_more_than_16_queues, TEST_FLAG_BLOCKING);

// ---------------------------------------------------------------------------
// Block layer
// ---------------------------------------------------------------------------
static void test_blk_init(void) {
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

// ---------------------------------------------------------------------------
// ATA (driver completo)
//
// [FIX] Todos los tests legacy de ATA usan test_skip() cuando no hay
// hda ni controlador IDE. En QEMU con solo AHCI, hda no existe y estos
// tests deben saltarse, no fallar.
// ---------------------------------------------------------------------------
static void test_ata_hda_detected(void) {
  // [FIX] Si no hay controlador IDE, skip.
  if (!ata_ide_present()) {
    test_skip("sin controlador IDE (hda no disponible)");
    return;
  }
  block_device_t *hda = blk_lookup("hda");
  TEST_ASSERT(hda != NULL, "hda no detectado");
}
REGISTER_TEST("ata: hda detectado", test_ata_hda_detected);

static void test_ata_hda_capacity(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    // [FIX] Skip en lugar de fail.
    test_skip("hda no existe");
    return;
  }
  TEST_ASSERT(hda->num_sectors > 0, "hda sin sectores");
  TEST_ASSERT(hda->sector_size == 512 || hda->sector_size == 4096,
              "sector_size inesperado: %u", hda->sector_size);
}
REGISTER_TEST("ata: capacidad de hda coherente", test_ata_hda_capacity);

static void test_ata_read_sector0(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }
  uint8_t buf[512];
  int rc = bdev_read(hda, 0, 1, buf);
  TEST_ASSERT(rc == 0, "read sector 0 falló: %d", rc);
  if (rc != 0)
    return;
  TEST_ASSERT(buf[510] == 0x55 && buf[511] == 0xAA,
              "sector 0 sin firma 0x55AA: %02x %02x", buf[510], buf[511]);
}
REGISTER_TEST("ata: leer sector 0 (firma 0x55AA)", test_ata_read_sector0);

static void test_ata_multi_sector_read(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }

  uint8_t *buf = (uint8_t *)kmalloc(512 * 4);
  TEST_ASSERT(buf != NULL, "kmalloc falló");
  if (!buf)
    return;

  int rc = bdev_read(hda, 0, 4, buf);
  TEST_ASSERT(rc == 0, "read 4 sectores falló: %d", rc);
  if (rc == 0) {
    TEST_ASSERT(buf[510] == 0x55 && buf[511] == 0xAA, "sector 0 sin firma");
  }
  kfree(buf);
}
REGISTER_TEST("ata: leer 4 sectores contiguos", test_ata_multi_sector_read);

static void test_ata_write_read_back(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }

  uint64_t lba = hda->num_sectors - 1;
  uint8_t orig[512], pattern[512], readback[512];

  int rc = bdev_read(hda, lba, 1, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    return;

  for (int i = 0; i < 512; i++)
    pattern[i] = (uint8_t)(i ^ 0xA5);

  rc = bdev_write(hda, lba, 1, pattern);
  TEST_ASSERT(rc == 0, "write falló: %d", rc);
  if (rc != 0)
    return;

  rc = bdev_flush(hda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);

  rc = bdev_read(hda, lba, 1, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    return;

  int ok = 1;
  for (int i = 0; i < 512; i++) {
    if (readback[i] != pattern[i]) {
      ok = 0;
      break;
    }
  }
  TEST_ASSERT(ok, "el patrón leído no coincide");

  bdev_write(hda, lba, 1, orig);
  bdev_flush(hda);
}
REGISTER_TEST("ata: escribir y leer de vuelta", test_ata_write_read_back);

static void test_ata_read_oob(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }
  uint8_t buf[512];
  int rc = bdev_read(hda, hda->num_sectors, 1, buf);
  TEST_ASSERT(rc < 0, "read fuera de rango debía fallar");
}
REGISTER_TEST("ata: leer fuera de rango falla", test_ata_read_oob);

static void test_ata_write_oob(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }
  uint8_t buf[512];
  int rc = bdev_write(hda, hda->num_sectors, 1, buf);
  TEST_ASSERT(rc < 0, "write fuera de rango debía fallar");
}
REGISTER_TEST("ata: escribir fuera de rango falla", test_ata_write_oob);

// ---------------------------------------------------------------------------
// ATAPI
//
// [FIX] Skip si sr0 no existe.
// ---------------------------------------------------------------------------
static void test_atapi_sr0_detected(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no detectado (sin unidad ATAPI)");
    return;
  }
  TEST_ASSERT(sr0 != NULL, "sr0 no detectado");
}
REGISTER_TEST("atapi: sr0 detectado", test_atapi_sr0_detected);

static void test_atapi_sr0_read_only(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  TEST_ASSERT(sr0->is_read_only == 1, "sr0 no es read-only");
}
REGISTER_TEST("atapi: sr0 read-only", test_atapi_sr0_read_only);

static void test_atapi_sr0_sector_size(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  TEST_ASSERT(sr0->sector_size == 2048 || sr0->sector_size == 4096,
              "sector_size inesperado: %u", sr0->sector_size);
}
REGISTER_TEST("atapi: sr0 sector size", test_atapi_sr0_sector_size);

static void test_atapi_sr0_media(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  if (sr0->num_sectors == 0) {
    TEST_ASSERT(1, "sr0 sin medio (OK si no hay CD)");
    return;
  }
  TEST_ASSERT(sr0->num_sectors > 0, "sr0 tiene medio pero 0 sectores");
}
REGISTER_TEST("atapi: sr0 medio", test_atapi_sr0_media);

static void test_atapi_read_pvd(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  if (sr0->num_sectors == 0) {
    TEST_ASSERT(1, "sr0 sin medio, test skip");
    return;
  }

  uint8_t *buf = (uint8_t *)kmalloc(sr0->sector_size);
  if (!buf) {
    TEST_ASSERT(0, "kmalloc falló");
    return;
  }

  int rc = bdev_read(sr0, 16, 1, buf);
  TEST_ASSERT(rc == 0, "read PVD falló: %d", rc);
  if (rc == 0) {
    TEST_ASSERT(buf[1] == 'C' && buf[2] == 'D' && buf[3] == '0' &&
                    buf[4] == '0' && buf[5] == '1',
                "PVD sin firma CD001: %02x %02x %02x %02x %02x", buf[1], buf[2],
                buf[3], buf[4], buf[5]);
  }
  kfree(buf);
}
REGISTER_TEST("atapi: leer PVD (firma CD001)", test_atapi_read_pvd);

static void test_atapi_write_fails(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  uint8_t *buf = (uint8_t *)kzalloc(sr0->sector_size ? sr0->sector_size : 2048);
  if (!buf) {
    TEST_ASSERT(0, "kzalloc falló");
    return;
  }
  int rc = bdev_write(sr0, 0, 1, buf);
  TEST_ASSERT(rc < 0, "write a sr0 debía fallar (read-only), rc=%d", rc);
  kfree(buf);
}
REGISTER_TEST("atapi: escribir falla (read-only)", test_atapi_write_fails);

static void test_atapi_read_oob(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  if (sr0->num_sectors == 0) {
    TEST_ASSERT(1, "sr0 sin medio, test skip");
    return;
  }
  uint8_t *buf = (uint8_t *)kmalloc(sr0->sector_size);
  if (!buf) {
    TEST_ASSERT(0, "kmalloc falló");
    return;
  }
  int rc = bdev_read(sr0, sr0->num_sectors, 1, buf);
  TEST_ASSERT(rc < 0, "read fuera de rango debía fallar");
  kfree(buf);
}
REGISTER_TEST("atapi: leer fuera de rango falla", test_atapi_read_oob);

// ---------------------------------------------------------------------------
// ATA DMA
//
// [FIX] Skip si el BMIDE no está inicializado (no hay controlador IDE).
// ---------------------------------------------------------------------------
static void test_ata_dma_bmide_ready(void) {
  extern int ata_dma_is_ready(int channel_idx);
  // [FIX] Si ninguno de los dos canales tiene BMIDE, skip.
  if (!ata_dma_is_ready(0) && !ata_dma_is_ready(1)) {
    test_skip("BMIDE no inicializado (sin controlador IDE)");
    return;
  }
  TEST_ASSERT(ata_dma_is_ready(0), "BMIDE primario no inicializado");
  TEST_ASSERT(ata_dma_is_ready(1), "BMIDE secundario no inicializado");
}
REGISTER_TEST("ata_dma: BMIDE listo", test_ata_dma_bmide_ready);

static void test_ata_dma_read_4k(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }

  uint8_t *buf1 = kmalloc(4096);
  uint8_t *buf2 = kmalloc(4096);
  if (!buf1 || !buf2) {
    TEST_ASSERT(0, "kmalloc falló");
    if (buf1)
      kfree(buf1);
    if (buf2)
      kfree(buf2);
    return;
  }

  int rc1 = bdev_read(hda, 0, 8, buf1);
  TEST_ASSERT(rc1 == 0, "read 4K (1) falló: %d", rc1);
  if (rc1 != 0) {
    kfree(buf1);
    kfree(buf2);
    return;
  }

  int rc2 = bdev_read(hda, 0, 8, buf2);
  TEST_ASSERT(rc2 == 0, "read 4K (2) falló: %d", rc2);
  if (rc2 != 0) {
    kfree(buf1);
    kfree(buf2);
    return;
  }

  int same = 1;
  for (int i = 0; i < 4096; i++) {
    if (buf1[i] != buf2[i]) {
      same = 0;
      break;
    }
  }
  TEST_ASSERT(same, "los dos buffers no coinciden");

  kfree(buf1);
  kfree(buf2);
}
REGISTER_TEST("ata_dma: leer 4KB", test_ata_dma_read_4k);

static void test_ata_dma_write_read(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }

  uint64_t lba = hda->num_sectors - 8;
  uint8_t *orig = kmalloc(4096);
  uint8_t *pattern = kmalloc(4096);
  uint8_t *readback = kmalloc(4096);
  if (!orig || !pattern || !readback) {
    TEST_ASSERT(0, "kmalloc falló");
    if (orig)
      kfree(orig);
    if (pattern)
      kfree(pattern);
    if (readback)
      kfree(readback);
    return;
  }

  int rc = bdev_read(hda, lba, 8, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    goto out;

  for (int i = 0; i < 4096; i++)
    pattern[i] = (uint8_t)(i ^ 0x5A);

  rc = bdev_write(hda, lba, 8, pattern);
  TEST_ASSERT(rc == 0, "write 4K falló: %d", rc);
  if (rc != 0)
    goto out;

  rc = bdev_flush(hda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);

  rc = bdev_read(hda, lba, 8, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    goto out;

  int same = 1;
  for (int i = 0; i < 4096; i++) {
    if (readback[i] != pattern[i]) {
      same = 0;
      break;
    }
  }
  TEST_ASSERT(same, "el patrón no coincide");

out:
  bdev_write(hda, lba, 8, orig);
  bdev_flush(hda);
  kfree(orig);
  kfree(pattern);
  kfree(readback);
}
REGISTER_TEST("ata_dma: escribir y leer 4KB", test_ata_dma_write_read);

// ---------------------------------------------------------------------------
// AHCI (Fase 3)
//
// Los tests buscan "sda", "sdb", ... en el block layer. Si no hay HBA AHCI
// o no hay discos SATA conectados, los tests pasan sin más (skip silencioso).
// ---------------------------------------------------------------------------
static void test_ahci_controller_detected(void) {
  extern int ahci_disk_count(void);
  int n = ahci_disk_count();
  LOG_INFO("  discos AHCI detectados: %d", n);
  TEST_ASSERT(n >= 0, "ahci_disk_count() devolvió %d", n);
}
REGISTER_TEST("ahci: controlador detectado", test_ahci_controller_detected);

static void test_ahci_sda_detected(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe (sin disco SATA), test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }
  TEST_ASSERT(sda->num_sectors > 0, "sda sin sectores");
  TEST_ASSERT(sda->sector_size == 512 || sda->sector_size == 4096,
              "sda sector_size inesperado: %u", sda->sector_size);
  TEST_ASSERT(sda->is_read_only == 0, "sda no debería ser read-only");
}
REGISTER_TEST("ahci: sda detectado", test_ahci_sda_detected);

static void test_ahci_read_sector0(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe, test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }

  uint8_t *buf = (uint8_t *)kmalloc(sda->sector_size);
  TEST_ASSERT(buf != NULL, "kmalloc falló");
  if (!buf)
    return;

  int rc = bdev_read(sda, 0, 1, buf);
  TEST_ASSERT(rc == 0, "read sector 0 falló: %d", rc);
  if (rc == 0) {
    LOG_INFO("  sector 0 leído OK (primeros bytes: %02x %02x %02x %02x)",
             buf[0], buf[1], buf[2], buf[3]);
  }
  kfree(buf);
}
REGISTER_TEST("ahci: leer sector 0 de sda", test_ahci_read_sector0);

static void test_ahci_read_4k(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe, test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }

  uint8_t *buf1 = (uint8_t *)kmalloc(4096);
  uint8_t *buf2 = (uint8_t *)kmalloc(4096);
  if (!buf1 || !buf2) {
    TEST_ASSERT(0, "kmalloc falló");
    if (buf1)
      kfree(buf1);
    if (buf2)
      kfree(buf2);
    return;
  }

  int rc1 = bdev_read(sda, 0, 8, buf1);
  TEST_ASSERT(rc1 == 0, "read 4K (1) falló: %d", rc1);
  if (rc1 != 0)
    goto out;

  int rc2 = bdev_read(sda, 0, 8, buf2);
  TEST_ASSERT(rc2 == 0, "read 4K (2) falló: %d", rc2);
  if (rc2 != 0)
    goto out;

  int same = 1;
  for (int i = 0; i < 4096; i++) {
    if (buf1[i] != buf2[i]) {
      same = 0;
      break;
    }
  }
  TEST_ASSERT(same, "las dos lecturas de 4K no coinciden");

out:
  kfree(buf1);
  kfree(buf2);
}
REGISTER_TEST("ahci: leer 4KB", test_ahci_read_4k);

static void test_ahci_write_read_back(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe, test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }
  if (sda->num_sectors < 16) {
    LOG_INFO("  sda demasiado pequeño, test omitido");
    TEST_ASSERT(1, "sda muy pequeño: skip");
    return;
  }

  uint64_t lba = sda->num_sectors - 8;
  uint8_t *orig = (uint8_t *)kmalloc(4096);
  uint8_t *pattern = (uint8_t *)kmalloc(4096);
  uint8_t *readback = (uint8_t *)kmalloc(4096);
  if (!orig || !pattern || !readback) {
    TEST_ASSERT(0, "kmalloc falló");
    if (orig)
      kfree(orig);
    if (pattern)
      kfree(pattern);
    if (readback)
      kfree(readback);
    return;
  }

  int rc = bdev_read(sda, lba, 8, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    goto out;

  for (int i = 0; i < 4096; i++)
    pattern[i] = (uint8_t)(i * 7 ^ 0x5A);

  rc = bdev_write(sda, lba, 8, pattern);
  TEST_ASSERT(rc == 0, "write 4K falló: %d", rc);
  if (rc != 0)
    goto out;

  rc = bdev_flush(sda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);

  rc = bdev_read(sda, lba, 8, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    goto out;

  int same = 1;
  for (int i = 0; i < 4096; i++) {
    if (readback[i] != pattern[i]) {
      same = 0;
      break;
    }
  }
  TEST_ASSERT(same, "el patrón leído no coincide tras write+read");

out:
  bdev_write(sda, lba, 8, orig);
  bdev_flush(sda);
  kfree(orig);
  kfree(pattern);
  kfree(readback);
}
REGISTER_TEST("ahci: escribir y leer 4KB", test_ahci_write_read_back);

static void test_ahci_read_oob(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe, test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }
  uint8_t buf[512];
  int rc = bdev_read(sda, sda->num_sectors, 1, buf);
  TEST_ASSERT(rc < 0, "read fuera de rango debía fallar, rc=%d", rc);
}
REGISTER_TEST("ahci: leer fuera de rango falla", test_ahci_read_oob);

// ===========================================================================
// ahci: latencia de comandos
// ===========================================================================
static void test_ahci_latencia(void) {
  block_device_t *bdev = blk_lookup("sda");
  if (!bdev) {
    test_skip("sin sda (no hay disco AHCI)");
    return;
  }

  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0) {
    test_skip("TSC no calibrado");
    return;
  }

  uint8_t *buf = kmalloc(4096);
  if (!buf) {
    TEST_ASSERT(0, "kmalloc(4096) devolvió NULL");
    return;
  }

  uint64_t t_min = (uint64_t)-1, t_max = 0, t_sum = 0;
  const int N = 32;
  int errores = 0;

  uint64_t lba_base = 100;
  if (lba_base + 8 > bdev->num_sectors)
    lba_base = 0;

  for (int i = 0; i < N; i++) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t0 = ((uint64_t)hi << 32) | lo;

    int rc = bdev_read(bdev, lba_base, 8, buf);

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t1 = ((uint64_t)hi << 32) | lo;

    if (rc < 0) {
      errores++;
      continue;
    }

    uint64_t dt = t1 - t0;
    if (dt < t_min)
      t_min = dt;
    if (dt > t_max)
      t_max = dt;
    t_sum += dt;
  }

  kfree(buf);

  if (errores > 0) {
    TEST_ASSERT(0, "%d/%d lecturas fallaron", errores, N);
    return;
  }

  uint64_t t_avg = t_sum / N;
  unsigned long avg_us = (unsigned long)(t_avg * 1000000ULL / freq);
  unsigned long min_us = (unsigned long)(t_min * 1000000ULL / freq);
  unsigned long max_us = (unsigned long)(t_max * 1000000ULL / freq);

  LOG_INFO("  latencia: min=%lu us, avg=%lu us, max=%lu us", min_us, avg_us,
           max_us);

  if (avg_us > 500) {
    LOG_WARN("  latencia alta: posiblemente IRQs enmascaradas (polling)");
  }

  TEST_ASSERT(avg_us > 0 && avg_us < 3000,
              "latencia media demasiado alta: %lu us (esperado < 500 us)",
              avg_us);
}

REGISTER_TEST_FLAGS("ahci: latencia", test_ahci_latencia, TEST_FLAG_BLOCKING);

// ===========================================================================
// ahci: stress 1MB write+read
//
// Escribe 1 MB (2048 sectores de 512 B) en una zona segura del disco,
// lo lee de vuelta, compara byte a byte, y restaura el contenido original.
//
// Ejercita:
//   - PRDT con múltiples entradas (1 MB / 64 KB = 16 comandos).
//   - Port_xfer dividiendo el bio en trozos de AHCI_MAX_XFER_BYTES.
//   - FLUSH tras la escritura (port_xfer lo hace).
//   - Lectura desde LBAs que no empiezan en 0.
//
// Es BLOCKING porque bdev_read/bdev_write pueden dormir en wait_event.
// ===========================================================================
static void test_ahci_stress_1mb(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    test_skip("sin sda (no hay disco AHCI)");
    return;
  }

  const uint32_t bytes = 1024 * 1024; // 1 MB
  const uint32_t sectors = bytes / sda->sector_size;

  if (sda->num_sectors < sectors + 4096) {
    test_skip("sda demasiado pequeño (%lu sectores) para el test",
              (unsigned long)sda->num_sectors);
    return;
  }

  // Zona segura: 2 MB antes del final, dejando margen para los otros
  // tests que escriben en num_sectors - 8.
  uint64_t lba = sda->num_sectors - sectors - 4096;

  uint8_t *orig = kmalloc(bytes);
  uint8_t *pattern = kmalloc(bytes);
  uint8_t *readback = kmalloc(bytes);
  if (!orig || !pattern || !readback) {
    TEST_ASSERT(0, "kmalloc(%u) falló (orig=%p pattern=%p readback=%p)", bytes,
                (void *)orig, (void *)pattern, (void *)readback);
    if (orig)
      kfree(orig);
    if (pattern)
      kfree(pattern);
    if (readback)
      kfree(readback);
    return;
  }

  int rc;

  // 1. Leer el contenido original para poder restaurarlo.
  rc = bdev_read(sda, lba, sectors, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    goto out;

  // 2. Generar un patrón determinista pero no trivial.
  //    Usamos un LCG simple para que sea reproducible.
  {
    uint32_t state = 0x12345678u;
    for (uint32_t i = 0; i < bytes; i++) {
      state = state * 1103515245u + 12345u;
      pattern[i] = (uint8_t)(state >> 16);
    }
  }

  // 3. Escribir 1 MB.
  LOG_INFO("  escribiendo %u bytes en lba=%lu", bytes, (unsigned long)lba);
  rc = bdev_write(sda, lba, sectors, pattern);
  TEST_ASSERT(rc == 0, "write 1MB falló: %d", rc);
  if (rc != 0)
    goto out;

  // 4. FLUSH (bdev_write ya llama a flush? No, lo hace port_xfer tras
  //    cada bio de escritura. Pero llamamos a bdev_flush explícitamente
  //    para asegurar que el disco persistió los datos.)
  rc = bdev_flush(sda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);
  if (rc != 0)
    goto out;

  // 5. Leer de vuelta y comparar.
  memset(readback, 0, bytes);
  rc = bdev_read(sda, lba, sectors, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    goto out;

  {
    uint32_t first_mismatch = UINT32_MAX;
    for (uint32_t i = 0; i < bytes; i++) {
      if (readback[i] != pattern[i]) {
        first_mismatch = i;
        break;
      }
    }
    if (first_mismatch != UINT32_MAX) {
      TEST_ASSERT(0, "mismatch en offset %u: esperado 0x%02x, leído 0x%02x",
                  first_mismatch, pattern[first_mismatch],
                  readback[first_mismatch]);
    } else {
      TEST_ASSERT(1, "1 MB escritos y leídos correctamente");
    }
  }

out:
  // Restaurar el contenido original, pase lo que pase.
  if (orig) {
    bdev_write(sda, lba, sectors, orig);
    bdev_flush(sda);
  }
  if (orig)
    kfree(orig);
  if (pattern)
    kfree(pattern);
  if (readback)
    kfree(readback);
}
REGISTER_TEST_FLAGS("ahci: stress 1MB write+read", test_ahci_stress_1mb,
                    TEST_FLAG_BLOCKING);

// ===========================================================================
// ahci: stress multiples LBAs
//
// Escribe un patrón distinto en 8 LBAs dispersos, verifica cada uno,
// y restaura. Valida que el driver funciona con LBAs no contiguos y
// que el FLUSH persiste cada escritura.
// ===========================================================================
static void test_ahci_stress_multi_lba(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    test_skip("sin sda (no hay disco AHCI)");
    return;
  }

  const uint32_t sectors_per_op = 8; // 4 KB por operación
  const int N = 8;
  const uint64_t margen_final = 8192;
  const uint64_t margen_inicial = 4096;

  if (sda->num_sectors < margen_inicial + margen_final + N * 4096) {
    test_skip("sda demasiado pequeño para el test");
    return;
  }

  uint32_t bytes = sectors_per_op * sda->sector_size;
  uint8_t *orig[N];
  uint8_t *pattern[N];
  uint8_t *readback[N];
  uint64_t lbas[N];

  for (int i = 0; i < N; i++) {
    orig[i] = NULL;
    pattern[i] = NULL;
    readback[i] = NULL;
  }

  // Elegir LBAs dispersos: espaciados ~4 MB, dentro de la zona segura.
  uint64_t zona = sda->num_sectors - margen_final - margen_inicial;
  for (int i = 0; i < N; i++) {
    lbas[i] = margen_inicial + (zona / N) * i;
    lbas[i] &= ~(uint64_t)7; // alinear a 8 sectores

    orig[i] = kmalloc(bytes);
    pattern[i] = kmalloc(bytes);
    readback[i] = kmalloc(bytes);
    if (!orig[i] || !pattern[i] || !readback[i]) {
      TEST_ASSERT(0, "kmalloc falló en iteración %d", i);
      goto cleanup;
    }
  }

  // Leer originales.
  for (int i = 0; i < N; i++) {
    int rc = bdev_read(sda, lbas[i], sectors_per_op, orig[i]);
    TEST_ASSERT(rc == 0, "read orig[%d] falló: %d", i, rc);
    if (rc != 0)
      goto cleanup;
  }

  // Generar patrones distintos y escribir.
  for (int i = 0; i < N; i++) {
    for (uint32_t j = 0; j < bytes; j++)
      pattern[i][j] = (uint8_t)((i * 31 + j) ^ 0xA5);

    int rc = bdev_write(sda, lbas[i], sectors_per_op, pattern[i]);
    TEST_ASSERT(rc == 0, "write[%d] falló: %d", i, rc);
    if (rc != 0)
      goto cleanup;
  }

  int rc = bdev_flush(sda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);
  if (rc != 0)
    goto cleanup;

  // Leer de vuelta y comparar.
  for (int i = 0; i < N; i++) {
    memset(readback[i], 0, bytes);
    rc = bdev_read(sda, lbas[i], sectors_per_op, readback[i]);
    TEST_ASSERT(rc == 0, "read back[%d] falló: %d", i, rc);
    if (rc != 0)
      goto cleanup;

    int ok = 1;
    uint32_t first_bad = 0;
    for (uint32_t j = 0; j < bytes; j++) {
      if (readback[i][j] != pattern[i][j]) {
        ok = 0;
        first_bad = j;
        break;
      }
    }
    if (!ok) {
      TEST_ASSERT(0,
                  "LBA %d (lba=%lu) mismatch en offset %u: "
                  "esperado 0x%02x, leído 0x%02x",
                  i, (unsigned long)lbas[i], first_bad, pattern[i][first_bad],
                  readback[i][first_bad]);
    } else {
      TEST_ASSERT(1, "LBA %d (lba=%lu) verificado OK", i,
                  (unsigned long)lbas[i]);
    }
  }

cleanup:
  // Restaurar todo.
  for (int i = 0; i < N; i++) {
    if (orig[i])
      bdev_write(sda, lbas[i], sectors_per_op, orig[i]);
  }
  bdev_flush(sda);
  for (int i = 0; i < N; i++) {
    if (orig[i])
      kfree(orig[i]);
    if (pattern[i])
      kfree(pattern[i]);
    if (readback[i])
      kfree(readback[i]);
  }
}
REGISTER_TEST_FLAGS("ahci: stress multiples LBAs", test_ahci_stress_multi_lba,
                    TEST_FLAG_BLOCKING);

// ===========================================================================
// ahci: stress buffer no contiguo
//
// Asigna un buffer grande (256 KB), comprueba cuántas páginas físicas
// distintas tiene, y hace write+read. Valida que el PRDT multi-entrada
// maneja buffers que no son contiguos en físico.
// ===========================================================================
static void test_ahci_stress_no_contiguo(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    test_skip("sin sda (no hay disco AHCI)");
    return;
  }

  const uint32_t bytes = 256 * 1024;
  const uint32_t sectors = bytes / sda->sector_size;

  if (sda->num_sectors < sectors + 8192) {
    test_skip("sda demasiado pequeño para el test");
    return;
  }

  uint64_t lba = sda->num_sectors - sectors - 8192;

  uint8_t *orig = kmalloc(bytes);
  uint8_t *pattern = kmalloc(bytes);
  uint8_t *readback = kmalloc(bytes);
  if (!orig || !pattern || !readback) {
    TEST_ASSERT(0, "kmalloc falló");
    if (orig)
      kfree(orig);
    if (pattern)
      kfree(pattern);
    if (readback)
      kfree(readback);
    return;
  }

  int rc;

  rc = bdev_read(sda, lba, sectors, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    goto out;

  // Contar páginas físicas distintas en el buffer.
  {
    extern uint64_t paging_get_phys(uint64_t virt);
    uint32_t paginas_distintas = 0;
    uint64_t last_phys = 0;
    int primera = 1;
    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE) {
      uint64_t phys = paging_get_phys((uint64_t)(uintptr_t)(pattern + off));
      if (primera || phys != last_phys + PAGE_SIZE) {
        paginas_distintas++;
      }
      last_phys = phys;
      primera = 0;
    }
    LOG_INFO("  buffer de %u bytes: %u tramos físicos contiguos", bytes,
             paginas_distintas);
    if (paginas_distintas == 1) {
      LOG_WARN("  el buffer es físicamente contiguo; el test no ejercita "
               "PRDT multi-entrada");
    }
  }

  for (uint32_t i = 0; i < bytes; i++)
    pattern[i] = (uint8_t)((i * 17) ^ 0x3C);

  LOG_INFO("  escribiendo %u bytes en lba=%lu", bytes, (unsigned long)lba);
  rc = bdev_write(sda, lba, sectors, pattern);
  TEST_ASSERT(rc == 0, "write falló: %d", rc);
  if (rc != 0)
    goto out;

  rc = bdev_flush(sda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);
  if (rc != 0)
    goto out;

  memset(readback, 0, bytes);
  rc = bdev_read(sda, lba, sectors, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    goto out;

  {
    uint32_t first_mismatch = UINT32_MAX;
    for (uint32_t i = 0; i < bytes; i++) {
      if (readback[i] != pattern[i]) {
        first_mismatch = i;
        break;
      }
    }
    if (first_mismatch != UINT32_MAX) {
      TEST_ASSERT(0, "mismatch en offset %u: esperado 0x%02x, leído 0x%02x",
                  first_mismatch, pattern[first_mismatch],
                  readback[first_mismatch]);
    } else {
      TEST_ASSERT(1, "buffer no contiguo escrito/leído OK");
    }
  }

out:
  if (orig) {
    bdev_write(sda, lba, sectors, orig);
    bdev_flush(sda);
  }
  if (orig)
    kfree(orig);
  if (pattern)
    kfree(pattern);
  if (readback)
    kfree(readback);
}
REGISTER_TEST_FLAGS("ahci: stress buffer no contiguo",
                    test_ahci_stress_no_contiguo, TEST_FLAG_BLOCKING);