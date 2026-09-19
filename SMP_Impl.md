## SMP (en progreso)

### Fase 0 — Infraestructura per-CPU [x]
- `cpu_local_t` con campos: kernel_stack, user_rsp, cpu_id, lapic_id,
  current_task, tick_counter.
- `cpu_local_data[MAX_CPUS]` con `MAX_CPUS = 8`.
- `smp_processor_id()` stub (devuelve 0).
- `this_cpu(field)` y `per_cpu(field, cpu)` macros.
- `smp_init()` inicializa cpu_local_data[0] con los valores de cpu_local.
- `smp_dump()` para debug.
- 4 tests nuevos: smp_processor_id, cpu_local_data[0], this_cpu, per_cpu.
- **Sin cambios funcionales**: el kernel sigue single-CPU.

### Fase 1 — ACPI MADT [x]
- Bootloader busca el RSDP en la config table de UEFI y lo copia
  por valor al `kernel_boot_info`.
- `acpi_init()` parsea el RSDP → XSDT/RSDT → MADT.
- Extrae: LAPIC base, IOAPIC base, CPUs (apic_id, processor_id,
  enabled), ISO mappings (IRQ legacy → GSI), BSP.
- `acpi_dump()` para debug.
- 4 tests nuevos. Total: 26 tests.
- **Sin cambios funcionales**: el kernel sigue usando el PIC 8259.
  Los APs no se arrancan todavía.

### Fase 2 — LAPIC + IOAPIC + LAPIC timer [x]

#### Sub-fase 2.1 — LAPIC [x]
#### Sub-fase 2.2 — IOAPIC [x]
#### Sub-fase 2.3 — LAPIC timer [x]
- Calibración del LAPIC timer contra el TSC.
- Modo periódico a 1 kHz.
- Vector 48 para el LAPIC timer.
- PIT desactivado (IRQ 0 enmascarada en el IOAPIC).
- `tick_count` y `timer_handler` movidos a `kernel/time.{h,c}`.
- Vector espurio 0xFF con handler real (`isr_spurious`).
- Total: 33 tests.

**Deuda técnica documentada** (ver sección aparte):
- EOI antes del handler (posible reentrada).
- LINT0 enmascarado (complicado volver al PIC).
- Colisión de GSI específica de QEMU.
- `cpu_local` vs `cpu_local_data[0]` desincronizados.

### Fase 3 — Arrancar APs [x]
- Trampoline en memoria baja (0x7000) en modo real -> protegido -> largo (64 bits).
- Rango [0x7000, 0x9000) reservado en el PMM antes de asignaciones.
- Secuencia INIT-SIPI-SIPI por hardware usando LAPIC ICR para despertar APs.
- ap_entry: inicializa CR0/CR4 (SSE/FPU), GDT, IDT, LAPIC local y %gs -> cpu_local_data[cpu].
- Sincronización con BSP vía smp_boot_params (aps_started, aps_ready).
- Estado verificado: 7/7 APs operativos en reposo (sti; hlt).

### Fase 4 — Scheduler SMP [/]

#### Sub-fase 4.1 — TSS per-CPU y migración de cpu_local [x]
- GDT ampliada para alojar 1 TSS por CPU (slots 5 + cpu*2).
- gdt_ap_init(cpu) carga TSS con ltr para cada CPU.
- tss_set_rsp0() per-CPU (this_cpu).
- syscall_entry.asm migrado a %gs: per-CPU con swapgs.
- Eliminación de la variable global cpu_local.

#### Sub-fase 4.2 — Scheduler Lock, current_task per-CPU y runqueue global [x]
- sched_lock (spinlock) protegiendo la runqueue y estructuras del scheduler.
- current_task migrado a this_cpu(current_task).
- Tareas idle dedicadas por CPU (idle_task[MAX_CPUS]).
- Prevenir que múltiples CPUs ejecuten la misma tarea (TASK_RUNNING).

#### Sub-fase 4.3 — Timer LAPIC en APs y despacho concurrente [x]
- Inicialización periódica del LAPIC timer en APs a 1 kHz (lapic_timer_init_ap).
- Separación de ticks: BSP incrementa tick_count global; cada CPU incrementa this_cpu(tick_counter) y llama a sched_tick().
- Los APs arrancan el scheduler con sched_start_ap(idle_task[my_cpu]).

#### Sub-fase 4.4 — IPIs de rescheduling remoto [x]
- Vectores IPI: 0xFB (resched), 0xFC (TLB), 0xFD (call), 0xFE (halt).
- `ipi.c`/`ipi.h`: `ipi_send`, `ipi_send_allbutself`, `ipi_send_all`.
- Handlers `ipi_handler_resched` y `ipi_handler_tlb` registrados en la IDT.
- Stubs en `isr_stubs.asm` con alineación de RSP robusta antes del call.
- `task_t.need_resched` + `sched_mark_need_resched()`.
- `idle_loop` comprueba `need_resched` antes de hlt.
- `sched_tick` fuerza el switch si `need_resched` está puesto, respetando
  `preempt_count`.
- `sched_make_ready` envía IPI a un AP idle cuando la tarea estaba
  bloqueada.

##### Sub-fase 4.4a — CPU affinity [x]
- Campo `int cpu_affinity` en `task_t` (-1 = cualquiera).
- `sched_tick` filtra tareas READY cuya afinidad no coincide con el CPU.
- `sched_make_ready` con afinidad explícita: solo IPI al CPU target.
- Las idle tasks tienen afinidad a su propio CPU.
- Tests: 36 en total. El test `smp: IPI wakeup cross-CPU` valida el flujo
  completo BSP → AP → BSP con afinidad.

### Fase 5 — Auditar drivers y subsistemas [ ]


### Deuda técnica — APIC/IOAPIC (post Fase 2)

#### EOI antes del handler (posible reentrada)
`irq_handler` envía EOI al LAPIC ANTES de llamar al handler. Si el
handler hace `sti` explícito o cambia a una tarea que lo haga, podría
reentrar. Es improbable con `IF=0` en el handler, pero anotado.
Fix futuro: EOI híbrido (al principio para timer, al final para
el resto).

#### LINT0 enmascarado
Con LINT0 enmascarado, el PIC no entrega IRQs al LAPIC. Es lo correcto
para IOAPIC, pero complica volver al PIC para depurar. Añadir una
flag `use_ioapic` que permita alternar.

#### Colisión de GSI específica de QEMU
La IRQ 2 (cascada al PIC slave) colisiona con IRQ 0 en GSI 2 por los
ISOs. La marcamos como no ruteable. En hardware real podría ser
necesario un tratamiento distinto. Revisar si aparece.

#### cpu_local vs cpu_local_data[0]
`cpu_local` y `cpu_local_data[0]` son dos instancias separadas. Se
sincronizan al arrancar. Cuando cambia `cpu_local.kernel_stack`, no se
actualiza `cpu_local_data[0]`. Migrar todo a `cpu_local_data[]` en
Fase 4.