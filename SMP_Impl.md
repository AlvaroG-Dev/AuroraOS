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


### Fase 2 — LAPIC + IOAPIC en el BSP [x]

#### Sub-fase 2.1 — LAPIC [x]
- LAPIC mapeado en MMIO_MAP_BASE.
- Habilitado vía MSR IA32_APIC_BASE (bit 11).
- SVR configurado con vector espurio 0xFF.
- LINT0 enmascarado (ya no recibimos IRQs del PIC).
- 3 tests nuevos (LAPIC mapeado, SVR, BSP ID).

#### Sub-fase 2.2 — IOAPIC [x]
- IOAPIC mapeado en MMIO_MAP_BASE.
- ISOs del MADT aplicados a la tabla IRQ → GSI.
- Detección de colisiones de GSI (IRQ 0 y IRQ 2 colisionan en GSI 2).
- Todas las IRQs del IOAPIC enmascaradas por defecto.
- PIC enmascarado completamente.
- `irq_install_handler` desenmascara en IOAPIC (no en PIC).
- `irq_handler` envía EOI al LAPIC **antes** del handler (evita perder
  el EOI cuando el handler cambia de tarea).
- 2 tests nuevos (IOAPIC detectado, ISO IRQ0->GSI2).
- Total: 31 tests.
### Fase 3 — Arrancar APs [ ]
### Fase 4 — Scheduler SMP [ ]
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