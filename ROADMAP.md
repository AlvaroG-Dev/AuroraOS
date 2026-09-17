# Aurora OS — Roadmap y mejoras pendientes

Estado a fecha de hoy. Los puntos ya completados se marcan con [x].

---

## Bloque A — Fundamentos pendientes

### A1. `\n` automático en `klog_printf` [x]
**Esfuerzo**: 30 min
**Por qué**: los logs se parten a mitad de mensaje cuando el usuario incluye `\n`.
**Cómo**: `klog_printf` añade `\n` al final; quitar los `\n` de todos los `LOG_*`.
Regla: una llamada = una línea.

### A2. Lock compartido entre `klog` y `serial` [x]
**Esfuerzo**: 1h
**Por qué**: stdout de usuario y logs del kernel se entrelazan.
**Cómo**: `serial_putc` coge un lock global. `klog` envuelve su mensaje entero
en el mismo lock (expón `serial_lock_acquire/release`).

### A3. Migrar `elf.c` y `sched.c` a `klog` [x]
**Esfuerzo**: 1h
**Por qué**: quedan `0x0x` y logs inconsistentes.
**Cómo**: `grep -n 'serial_' elf.c sched.c` y migrar todo a `LOG_*`.

### A4. TSC para timestamps [x]
**Esfuerzo**: 2h
**Por qué**: los timestamps `[0.000]` durante el boot son inútiles.
**Cómo**: leer `rdtsc()` en `klog.c` en vez de `tick_count`. Calibrar la
frecuencia contra el PIT tras `pit_init`.

---

## Bloque B — Estabilidad y robustez

### B1. `#PF` handler con demand paging [x]
**Esfuerzo**: 1 semana
**Por qué**: hoy el handler sólo imprime y cuelga.
**Cómo**:
- En `isr_handler` para vector 14, llamar a `handle_page_fault(err, cr2)`.
- Si `cr2` está en rango válido (heap, stack, mmap), mapear página y `iretq`.
- Si no, panic.
- Añadir `SYS_MMAP` para regiones válidas.

### B2. NX + SMEP + SMAP [x]
**Esfuerzo**: 3h
**Por qué**: hoy no activas NX. El usuario puede ejecutar datos.
**Cómo**:
- `EFER.NXE = 1` en `paging_init`.
- Páginas de datos con `PTE_NX`.
- `CR4.SMEP = 1`, `CR4.SMAP = 1`.
- `stac`/`clac` alrededor de `copy_from_user`/`copy_to_user`.

### B3. Guard pages en stacks [x]
**Esfuerzo**: 2h
**Por qué**: un stack overflow corrompe silenciosamente.
**Cómo**: reserva una página no mapeada debajo del stack de cada tarea.
Cuando el stack crezca demasiado, `#PF` en la guard page.

### B4. Backtrace en panic [x]
**Esfuerzo**: 3h
**Por qué**: cuando crashee algo, querrás saber de dónde viene.
**Cómo**: caminar `RBP` hacia arriba en `isr_handler`, imprimir los `RIP`.

### B5. `kfree` de bloques no del heap [x]
**Esfuerzo**: 2h
**Por qué**: hoy sólo validas `magic`; puede coincidir por casualidad.
**Cómo**: valida que `blk` esté dentro de `[HEAP_VMA, heap_top)`.

---

## Bloque C — Rendimiento y escalabilidad

### C1. SLAB allocator para objetos pequeños [x]
**Esfuerzo**: 1 semana
**Por qué**: `kmalloc` fragmenta y es O(n) por asignación.
**Cómo**: caches por tamaño (32/64/128/256/512/1024/2048) + free lists.
El allocator actual queda para bloques grandes.

### C2. `kzalloc`, `krealloc`, `kcalloc`, `kstrdup` [x]
**Esfuerzo**: 2h
**Por qué**: los usas implícitamente en todo el código.
**Cómo**: wrappers sobre `kmalloc`.

### C3. Idle task real con `hlt` [x]
**Esfuerzo**: 2h
**Por qué**: el `while(1){...hlt;}` final debería ser una tarea del scheduler.
**Cómo**: `sched_create_idle_task()` que hace `for(;;) hlt;`.

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
**Hacer después de PHYS_MAP_BASE.**

### D2. Storage: driver AHCI o virtio-blk [ ]
**Esfuerzo**: 1-2 semanas
**Por qué**: nada persiste.
**Cómo**: PCI → BAR → MMIO. Detectar SATA, leer/escribir sectores.
Añadir a VFS como `/dev/sda`.

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
**Esfuerzo**: 1 semana
**Por qué**: consultar estado del kernel desde usuario.
**Cómo**: VFS virtual que genera contenido on-read.
`/proc/meminfo`, `/proc/tasks`, `/proc/heap`, `/dev/kmsg`.

### E2. Tests formales en el kernel [x]
**Esfuerzo**: 2 días
**Por qué**: ya tienes `pmm_self_test` y `[HEAP-TEST]`. Formalízalos.
**Cómo**: `TEST_ASSERT(cond, msg)` + runner que ejecute tests al boot.

### E3. Comandos de debug en panic [x]
**Esfuerzo**: 1 día
**Por qué**: cuando crashee, querrás dumpear heap, tareas, tablas de páginas.
**Cómo**: en `isr_handler`, llamar a `heap_dump()`, `sched_dump()`,
`paging_dump()`.

### E4. CI con GitHub Actions [x]
**Esfuerzo**: 1 día
**Por qué**: cada commit compila y arranca en QEMU headless.
**Cómo**: workflow con `x86_64-elf-gcc` + QEMU, `-nographic`,
capturar serial y verificar cadenas esperadas.

---

## Bloque F — Refactors y limpieza

### F1. `string.c` propio [x]
**Esfuerzo**: 2h
**Por qué**: tienes `memcpy` en `main.c`, `__builtin_memset` por todos lados.
**Cómo**: `memset`, `memcpy`, `memmove`, `memcmp`, `strlen`, `strcmp`,
`strncmp`, `strcpy`, `strncpy`, `strchr`. Usa `-ffreestanding`.

### F2. `printk` formatters para tipos específicos [ ]
**Esfuerzo**: 1h
**Por qué**: `%p` está bien, pero podrías tener `%MAC`, `%IP`, `%UUID`.

### F3. `panic()` centralizado [x]
**Esfuerzo**: 2h
**Por qué**: hoy `isr_handler` hace todo. Separa `panic(fmt, ...)`.

### F4. Framebuffer console [ ]
**Esfuerzo**: 1 día
**Por qué**: hoy `fb_puts` es rudimentario.
**Cómo**: ring de líneas en compositor, `klog` escribe a serial + framebuffer.

---

## Bloque G — Refactors pequeños (commit suelto)

- [x] `idt.c`: bucle con array de ISRs en vez de 48 líneas manuales.
- [x] `MSR_SFMASK = 0x257C` — comentar qué bits son.
- [x] `syscall_init` se llama dos veces — quitar la segunda.
- [x] `pmm.c`: `bitmap` sin lock en lecturas — añadir `_Atomic` o lock.
- [x] `vfs.c`: `vfs_lookup` no libera `node` si el llamante falla.
- [x] `tarfs.c`: `MAX_NODES 256` — avisar si se trunca.
- [ ] `User apps`: Warnings de RWX segments

---

## Ya completado [x]

- [x] Bugs del heap (§1.1–1.4): alineación, magic, locking.
- [x] Doble `0x` en `serial_hex` (parcial — quedan en `elf.c`, `sched.c`).
- [x] `klog` unificado con niveles.
- [x] `spinlock` en el heap.
- [x] `preempt_disable` en `SYS_SPAWN` (parche provisional).
- [x] PHYS_MAP_BASE (plan en `docs/PHYS_MAP_BASE_Plan.md`, pendiente ejecutar).

---

## Pre-SMP (nuevo, fuera del ROADMAP original)
- [x] preempt_count por tarea + preempt_disable/enable.
- [x] Invariantes del scheduler (BLOCKED ↔ waiting_on).
- [x] Fix task_entry_wrapper (cli alrededor de state=DEAD).
- [x] preempt_disable en compositor (window_stack).
- [x] preempt_disable en process_list.

## TTY mínimo + shell interactivo [x]

- kernel/tty.c: TTY con line discipline (buffer de línea + backspace + eco).
- Nodo VFS /dev/tty expuesto por tty.c (tty_get_node).
- stdin (fd 0) apunta a /dev/tty: read() bloquea hasta línea completa.
- PS/2 traduce scancode → ASCII y entrega al TTY.
- user/apps/shell: shell interactivo con help/echo/cat/exit/spawn.
- Ciclo completo: tecla → IRQ → TTY → wake_up → shell.

---

## Deuda técnica — arranque de kmain (para SMP)

### kmain no es una tarea del scheduler
`kmain` corre desde el bootloader con el stack de la idle task,
pero `current_task` apunta a otras tareas después del primer tick.
Cuando el timer desaloja, `task_switch` guarda el contexto de kmain
como si fuera el de `idle`. Al volver, `kmain` continúa donde lo dejó.

Esto **funciona** hoy porque:
- `kmain` no comparte recursos con las tareas que desaloja.
- Las tareas se crean en orden tal que ninguna depende de otra
  para arrancar.

Pero es **frágil**:
- Si un test hace `wait_event()` con `preempt_disable`, deadlock.
- Si `sti` se mueve antes, `sched_tick` puede desalojar a kmain
  mientras modifica estructuras (TARFS, heap, paging).

### Fix recomendado (hacer antes de SMP)
Convertir `kmain` en una tarea real:
```c
void kmain_task(void) {
    kmain_body();       // todo el setup actual
    while (1) sched_yield();
}
```

En el entrypoint: sched_create_task(kmain_task); sched_yield();
Así kmain es una tarea más, con su stack, y el sti puede ir
donde convenga sin sorpresas. Es el patrón init_task de Linux.

### Fix intermedio (si no quieres reescribir todavía)
Mover run_all_tests() a justo antes del while(1) final de
kmain, después de todo el setup. Y dejar sti donde está.
Así los tests corren como "la última cosa que hace kmain antes
de ceder para siempre". Menos elegante pero elimina la mayor
parte del riesgo.

### Lo que no hay que hacer
No hacer preempt_disable() global alrededor de todo el setup
porque klog_calibrate_tsc necesita IRQs activas para funcionar
(hace hlt esperando ticks). Y ps2_thread necesita correr.

## Deuda técnica conocida (para futuras iteraciones)

### heap_dump / slab_dump y locks
`heap_dump()` y `slab_dump_stats()` consultan `panic_in_progress()`.
Si están en panic, no cogen locks. Esto es correcto single-CPU pero
puede dar falsos negativos en SMP (otra CPU podría tener el lock
realmente cogido y el dump leería estado inconsistente). Cuando
lleguemos a SMP, revisar este diseño: tal vez usar trylock con
timeout, o un snapshot atómico del estado.

### serial_lock no se resetea en panic
Si el panic ocurre mientras un `LOG_*` está escribiendo a serial
(con `serial_lock` cogido), el dump del panic se cuelga en el
primer `LOG_INFO`. Mitigación actual: no hacer LOG durante el
propio panic (usamos `serial_puts`/`serial_hex` directos en
`panic.c`). Pero los dumps (`dump_scheduler`, `dump_heap`, etc.)
usan `LOG_*`... inconsistente. Unificar: usar serial directo en
todos los dumps, o añadir `serial_lock_reset()` que se llame en
`panic_v` antes de los dumps. Anotado.

### `HEAP_MAX_SINGLE_ALLOC = 64 MB`
Límite arbitrario. Si en el futuro algún subsistema legítimamente
necesita `kmalloc` de más de 64 MB, subir el límite o
implementar un `vmalloc()` (bloques grandes fuera del heap
lineal). Hoy no hace falta.

**Y sobre los tests**, una nota en el bloque E2 del ROADMAP:

### E2. Tests formales en el kernel [x]
...
**Notas**:
- El linker coloca las entradas de `.tests` en orden **inverso**
  al de declaración dentro de cada `.o`. Los tests deben ser
  independientes entre sí. Si algún día hace falta orden, añadir
  un campo `order` y ordenar en `run_all_tests`.
- Los tests corren con `preempt_disable()` para que el scheduler
  no los desaloje a mitad. **Ningún test debe bloquearse** (nada
  de `wait_event`, `sched_yield` cooperativo, ni `hlt`). Si algún
  día hace falta, añadir un flag `TEST_FLAG_BLOCKING` y tratarlo
  distinto.
- Los tests corren con `sti` habilitado y el compositor ya
  corriendo. Un test falla de forma determinista solo si toca
  estado que él mismo inicializa. No asumir orden ni aislamiento
  del resto del kernel.

## Priorización recomendada

### Ahora (esta semana)
1. A1 — `\n` automático en klog (30 min)
2. A2 — lock compartido klog/serial (1h)
3. A3 — migrar `elf.c`, `sched.c` a klog (1h)
4. A4 — TSC para timestamps (2h)
5. B2 — NX + SMEP + SMAP (3h)

### Próximas 2 semanas
6. PHYS_MAP_BASE (una semana)
7. B1 — `#PF` handler con demand paging (1 semana)

### Mes siguiente
8. C1 — SLAB (1 semana)
9. C3 — idle task real (2h)
10. B3 — guard pages (2h)
11. E2 — tests formales (2 días)
12. E4 — CI (1 día)

### Trimestre siguiente
13. D1 — SMP + APIC (2-3 semanas)
14. D2 — AHCI (1-2 semanas)
15. D3 — FAT32 (1-2 semanas)
16. D4 — Red (2-3 semanas)
17. D5 — USB (3-4 semanas)

Los bloques E y F son transversales: hazlos cuando tengas un hueco.
Los refactors pequeños (G) los puedes ir metiendo en commits sueltos.