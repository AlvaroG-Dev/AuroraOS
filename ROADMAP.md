# Aurora OS — Roadmap y mejoras pendientes

Estado a fecha de hoy. Los puntos ya completados se marcan con [x].

**Estado del proyecto**: kernel x86_64 bare-metal que arranca por UEFI,
con scheduler preemptivo, allocadores (heap + SLAB), VFS sobre TarFS,
IPC por mailboxes, TTY con shell interactivo, CI en GitHub Actions.
Los subsistemas de storage, red y USB aún no existen (bloque D).
SMP es el siguiente gran salto (bloque D1).

---

## Bloque A — Fundamentos

### A1. `\n` automático en `klog_printf` [x]
**Esfuerzo**: 30 min
**Hecho**: `klog_printf` añade `\n` al final; los `LOG_*` ya no lo
llevan. Regla: una llamada = una línea.

### A2. Lock compartido entre `klog` y `serial` [x]
**Hecho**: `serial_lock_acquire/release` expuestos. `klog` envuelve
su mensaje completo en el mismo lock. stdout de usuario y logs del
kernel no se entrelazan.

### A3. Migrar `elf.c` y `sched.c` a `klog` [x]
**Hecho**: no quedan `serial_*` en esos archivos.

### A4. TSC para timestamps [x]
**Hecho**: `klog.c` lee `rdtsc()` relativo a `tsc_origin`. Frecuencia
calibrada contra el PIT tras `pit_init` (`klog_calibrate_tsc`).

---

## Bloque B — Estabilidad y robustez

### B1. `#PF` handler con demand paging [x]
**Hecho**:
- `handle_page_fault(err, regs)` en `pf.c`.
- Heap, stack, mmap como regiones válidas con VMAs.
- `SYS_MMAP` / `SYS_MUNMAP` implementadas.
- Kill de la tarea si el acceso no es a un VMA o viola permisos.

### B2. NX + SMEP + SMAP [x]
**Hecho**:
- `EFER.NXE = 1` en `paging_init`.
- PTE_NX en páginas de datos.
- `CR4.SMEP = 1`, `CR4.SMAP = 1` (con detección CPUID).
- `stac`/`clac` alrededor de accesos a userland.

### B3. Guard pages en stacks [x]
**Hecho**: `proc->stack_guard` reserva una página no mapeada debajo
del stack. `segvtest_stack` la dispara correctamente.

### B4. Backtrace en panic [x]
**Hecho**: `backtrace(rbp, rip, max)` recorre la cadena de RBP y
valida que cada RIP caiga en `[__text_start, __text_end)`.

### B5. `kfree` de bloques no del heap [x]
**Hecho**: se valida que `blk` esté en `[HEAP_VMA, heap_top)` antes
de tocar magic.

---

## Bloque C — Rendimiento y escalabilidad

### C1. SLAB allocator para objetos pequeños [x]
**Esfuerzo**: 1 semana
**Hecho**:
- Región virtual dedicada `SLAB_VMA = 0xFFFFFFFF83000000`.
- Caches: 16, 32, 64, 128, 256, 512, 1024, 2048.
- Slabs de una página, header de 72 bytes.
- Lock por cache + `slab_top_lock` para el bump pointer.
- Slabs completamente vacíos se devuelven al PMM (salvo el último).
- `kmalloc(size <= 2048)` → SLAB. Resto → heap first-fit.
- `krealloc` con fast path SLAB → SLAB en el mismo cache.
- 6 tests en el framework `.tests`. Total: 18 tests.

### C2. `kzalloc`, `krealloc`, `kcalloc`, `kstrdup` [x]
**Hecho**: wrappers sobre `kmalloc` con overflow checks donde toca.

### C3. Idle task real con `hlt` [x]
**Hecho**: `sched_create_task(idle_loop)` con `idle->is_idle = 1`.
El scheduler nunca la reapea.

---

## Bloque D — Subsistemas que faltan

### D1. SMP + APIC + IOAPIC + LAPIC timer [ ]
**Esfuerzo**: 2-3 semanas
**Por qué**: estás atado a un solo CPU y al PIC 8259.
**Cómo**:
- Parsear ACPI MADT para LAPIC/IOAPIC.
- Sustituir PIC por IOAPIC.
- LAPIC timer en vez de PIT.
- IPIs para TLB shootdown y resched remoto.
- Arrancar APs vía INIT-SIPI-SIPI.
- `per_cpu` con `gs:` base distinta por CPU.
**Pre-requisitos** (ver "Deuda técnica"): refcount en `task_t`,
`smp_processor_id` stub, revisar `kmain` como tarea real.

### D2. Storage: driver AHCI o virtio-blk [ ]
**Esfuerzo**: 1-2 semanas
**Por qué**: nada persiste.
**Cómo**: PCI → BAR → MMIO. Detectar SATA, leer/escribir sectores.
Añadir a VFS como `/dev/sda`. Necesita wait queue de completación.

### D3. FAT32 o ext2 [ ]
**Esfuerzo**: 1-2 semanas
**Por qué**: tras el driver de bloque, necesitas FS de escritura.
**Cómo**: FAT32 primero en RAM disk, luego migrar a AHCI.

### D4. Red: e1000 + pila TCP/IP [ ]
**Esfuerzo**: 2-3 semanas
**Por qué**: `ping`, HTTP mínimo.
**Cómo**: driver e1000, luego ARP → IPv4 → ICMP → UDP → TCP.
Sockets como syscalls.

### D5. USB: xHCI o EHCI [ ]
**Esfuerzo**: 3-4 semanas
**Por qué**: sin USB no hay teclado/ratón en hardware moderno.
**Cómo**: enumeración, transferencias de control, luego HID.

---

## Bloque E — Herramientas y calidad

### E1. `/proc` y `/dev/kmsg` [ ]
**Esfuerzo**: 1 semana (menos con las wq ya hechas)
**Por qué**: consultar estado del kernel desde usuario.
**Cómo**: VFS virtual que genera contenido on-read.
`/proc/meminfo`, `/proc/tasks`, `/proc/heap`, `/dev/kmsg`.
`/dev/kmsg` usa una wait queue que `klog_printf` despierta.

### E2. Tests formales en el kernel [x]
**Hecho**:
- `TEST_ASSERT(cond, fmt, ...)` + `REGISTER_TEST(name, fn)`.
- Sección `.tests` del linker script para autodescubrimiento.
- `run_all_tests()` corre con `preempt_disable()` por test.
- 18 tests (heap + slab). Resumen al final con pass/fail/skip.

**Notas**:
- El linker coloca las entradas de `.tests` en orden inverso al de
  declaración dentro de cada `.o`. Los tests deben ser independientes.
- **Ningún test debe bloquearse** (nada de `wait_event`, `sched_yield`
  cooperativo, ni `hlt`). Si algún día hace falta, añadir un flag
  `TEST_FLAG_BLOCKING` y tratarlo distinto.
- Corren con `sti` habilitado y el compositor ya corriendo. Un test
  solo es determinista si toca estado que él mismo inicializa.

### E3. Comandos de debug en panic [x]
**Hecho**:
- `panic()` centralizado + `panic_noctx(fmt, ...)`.
- `panic_in_progress()` expuesto. Los dumps no cogen locks si estamos
  en panic.
- `dump_registers(regs)`, `dump_scheduler()`, `dump_paging()`,
  `dump_slab()`, `dump_heap()`.
- `dump_scheduler` con chequeo de rango defensivo antes de
  dereferenciar (evita #PF dentro del panic).
- `isr_handler` reducido a ~10 líneas: `handle_page_fault` o `panic`.

**Notas**:
- `heap_dump` y `slab_dump_stats` consultan `panic_in_progress()`.
- `serial_lock` no se resetea en panic. Un panic dentro de un `LOG_*`
  puede colgarse. Anotado como deuda técnica.
- `task_t` no tiene `name`. El dump del scheduler solo muestra
  `id`/`state`/`wait`/`idle`. Añadir nombre legible es mejora futura.

### E4. CI con GitHub Actions [x]
**Hecho**:
- `.github/workflows/build.yml`: compila bootloader + kernel + user
  + ISO. Sube `aurora-iso` como artefacto. Corre en cada push/PR.
- `.github/workflows/boot.yml`: arranca QEMU headless, captura serial
  a `serial.log`, verifica 14 cadenas esperadas con
  `scripts/check_boot.sh`. Si falla, sube `serial.log` y `qemu.log`.
- `bootloader/Makefile`: detección robusta de gnu-efi (pkg-config +
  fallback a paths conocidos). Funciona en CI limpio y en local.

**Decisiones**:
- En boot solo se carga `apps/shell` (no `apps/hello`). Los 18 tests
  del kernel cubren heap + slab. Las 11 pruebas de `hello` se corren
  manualmente arrancando `apps/hello` desde el shell.

**Pendiente**:
- Actualizar `actions/checkout@v4` → `@v5` cuando sea posible
  (Node.js 20 deprecation warning).

---

## Bloque F — Refactors y limpieza

### F1. `string.c` propio [x]
**Hecho**: `memset`, `memcpy`, `memmove`, `memcmp`, `strlen`, `strcmp`,
`strncmp`, `strcpy`, `strncpy`, `strchr`. `-ffreestanding`.

### F2. `printk` formatters para tipos específicos [ ]
**Esfuerzo**: 1h
**Por qué**: `%p` está bien, pero podrías tener `%MAC`, `%IP`, `%UUID`.
Utilidad real baja. Hacer cuando llegue red (D4).

### F3. `panic()` centralizado [x]
**Hecho**: ver E3.

### F4. Framebuffer console [ ]
**Esfuerzo**: 1 día
**Por qué**: hoy el TTY escribe solo a serial. La demo no es visible
en la ventana gráfica de QEMU.
**Cómo**: ring de líneas en el compositor, `tty_echo` escribe a serial
**y** al framebuffer. Cursor parpadeante.

---

## Bloque G — Refactors pequeños

- [x] `idt.c`: bucle con array de ISRs en vez de 48 líneas manuales.
- [x] `MSR_SFMASK = 0x257C` — comentar qué bits son.
- [x] `syscall_init` se llama dos veces — verificado: falso positivo.
- [x] `pmm.c`: `bitmap` sin lock en lecturas — ya tenía lock, cerrado.
- [x] `vfs.c`: `vfs_lookup` no libera `node` si el llamante falla.
- [x] `tarfs.c`: `MAX_NODES 256` — aviso si se trunca.
- [ ] User apps: Warnings de RWX segments (diferido a cuando toquemos
  el build de userland de forma seria).

---

## Pre-SMP (completado)

- [x] `preempt_count` por tarea + `preempt_disable/enable`.
- [x] Invariantes del scheduler (BLOCKED ↔ waiting_on != NULL).
- [x] Fix `task_entry_wrapper` (cli alrededor de state=DEAD).
- [x] `preempt_disable` en compositor (`window_stack`).
- [x] `preempt_disable` en `process_list`.

---

## TTY mínimo + shell interactivo [x]

- `kernel/tty.c`: TTY con line discipline (buffer de línea + backspace
  + eco). Expone `tty_get_node()` con ops VFS.
- `stdin` (fd 0) apunta a `/dev/tty`: `read()` bloquea hasta línea
  completa.
- PS/2 traduce scancode → ASCII y entrega al TTY.
- `user/apps/shell`: shell interactivo con `help`/`echo`/`cat`/`exit`/
  `spawn`.
- Ciclo completo: tecla → IRQ → TTY → `wake_up` → shell.

---

## Wait queues end-to-end [x]

- `wait.h`/`wait.c`: `wait_event`, `wait_event_interruptible`,
  `wake_up_all`, `wake_up_one`.
- `sched.h`/`sched.c`: `waiting_on`, `wake_reason`, `sched_make_ready`.
  `sched_unblock` eliminado.
- `ipc.c`: cada mailbox con su wq. `ipc_recv` bloqueante duerme ahí.
- `process.c`: cada proceso con `child_wq`. `waitpid` bloqueante.
- `vfs.c`: cada fd con `read_wq`/`write_wq`. `vfs_wait_readable`.
- `ps2.c`, `compositor.c`: sin polling, todo por wq.

---

## Deuda técnica conocida

### kmain no es una tarea del scheduler
`kmain` corre desde el bootloader con el stack de la idle task, pero
`current_task` apunta a otras tareas después del primer tick. Cuando
el timer desaloja, `task_switch` guarda el contexto de kmain como si
fuera el de `idle`. Al volver, `kmain` continúa donde lo dejó.

Funciona hoy porque `kmain` no comparte recursos con las tareas que
desaloja, y las tareas se crean en orden tal que ninguna depende de
otra para arrancar.

**Fix recomendado (antes de SMP)**: convertir `kmain` en una tarea
real:
```c
void kmain_task(void) {
    kmain_body();
    while (1) sched_yield();
}
// En el entrypoint:
sched_create_task(kmain_task);
sched_yield();
Es el patrón init_task de Linux. Pendiente.

heap_dump / slab_dump y locks
heap_dump() y slab_dump_stats() consultan panic_in_progress().
Si están en panic, no cogen locks. Correcto single-CPU. En SMP puede
dar falsos negativos (otra CPU con el lock cogido → dump inconsistente).
Revisar al hacer SMP. Opciones: trylock con timeout, o snapshot
atómico del estado.

serial_lock no se resetea en panic
Si el panic ocurre mientras un LOG_* está escribiendo a serial (con
serial_lock cogido), el dump del panic se cuelga en el primer
LOG_INFO. Mitigación actual: en panic.c usamos serial_* directos,
pero los dumps siguen usando LOG_* → inconsistente.
Fix futuro: unificar a serial directo en todos los dumps, o añadir
serial_lock_reset() que se llame en panic_v antes de los dumps.

HEAP_MAX_SINGLE_ALLOC = 64 MB
Límite arbitrario. Si algún subsistema legítimamente necesita kmalloc
de más de 64 MB, subir el límite o implementar vmalloc() (bloques
grandes fuera del heap lineal). Hoy no hace falta.

dump_scheduler no muestra nombre de tarea
task_t no tiene campo name. Añadir para debug (especialmente útil
cuando SMP meta tareas AP). Bajo prioridad.

task_t sin refcount
Cuando SMP entre, una tarea puede ser liberada mientras otra CPU aún
tiene un puntero a ella (por ejemplo, en una wait queue). Añadir
refcount atómico antes de SMP.

smp_processor_id() stub
Hoy no existe. Aunque sea single-CPU, definir
static inline int smp_processor_id(void) { return 0; } y
per_cpu macros prepara el código nuevo para SMP sin refactorizar
después. Bajo coste, hacer antes de D1.

Orden de tests en .tests
El linker invierte el orden dentro de cada .o. No es un bug, pero
los tests deben ser independientes. Anotado para futura referencia.

Priorización recomendada (revisada)
Próxima sesión
F4 — Framebuffer console (1 día). Alto impacto visual.
El TTY escribe a serial + framebuffer.

smp_processor_id stub + per_cpu macros (3h). Pre-SMP barato.

Siguientes 2 semanas
E1 — /proc y /dev/kmsg (3-4 días). Cierra más ciclos de
wait queues y da visibilidad al usuario.

kmain como tarea real (4h). Pre-SMP.

refcount en task_t (4h). Pre-SMP.

F3 (parcial) — unificar serial_lock en panic (2h).

vmalloc() para bloques >64 MB (1 día). Opcional.

Mes siguiente
D1 — SMP + APIC (2-3 semanas). El gran salto.

Trimestre siguiente
D2 — AHCI (1-2 semanas)

D3 — FAT32 (1-2 semanas)

D4 — Red (2-3 semanas)

D5 — USB (3-4 semanas)

Los bloques E y F son transversales. Los refactors pequeños de G los
puedes meter en commits sueltos cuando surjan.

Lo que ya está completado (referencia histórica)
☑ Bugs del heap (§1.1–1.4): alineación, magic, locking.
☑ Doble 0x en serial_hex.
☑ klog unificado con niveles.
☑ spinlock en el heap.
☑ preempt_disable en SYS_SPAWN (parche provisional, ahora
cubierto por preempt_disable general en compositor y process_list).
☑ PHYS_MAP_BASE + MMIO_MAP_BASE.