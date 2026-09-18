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
### Fase 2 — LAPIC + IOAPIC en el BSP [ ]
### Fase 3 — Arrancar APs [ ]
### Fase 4 — Scheduler SMP [ ]
### Fase 5 — Auditar drivers y subsistemas [ ]