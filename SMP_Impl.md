Plan de SMP por fases
Cada fase es compilable y testeable por sí sola. Si una rompe algo, sabes dónde está el problema.

Fase 0 (esta): inventario y refactor mínimo
Auditar todo el código que asume single-CPU.

Identificar qué rompe con SMP y qué no.

Añadir la infraestructura base (constantes, structs, stubs) sin tocar nada funcional.

Compilar y verificar que el CI sigue verde.

Coste: 1-2 días. Riesgo: nulo.

Fase 1: ACPI MADT — descubrir CPUs y APIC IDs
Parsear las tablas ACPI para encontrar el MADT (Multiple APIC Description Table).

Extraer: LAPIC base, IOAPIC base, lista de CPUs (con sus APIC IDs).

Sin activar nada. Solo descubrir y loguear.

Coste: 2-3 días. Riesgo: bajo. Solo toca ACPI, que es un parser de tablas.

Fase 2: LAPIC en el BSP
Habilitar el LAPIC del BSP (Bootstrap Processor).

Sustituir el PIC 8259 por el IOAPIC.

Usar el LAPIC timer en vez del PIT.

Sigue habiendo una sola CPU ejecutando tareas. Las demás están dormidas.

Coste: 3-4 días. Riesgo: medio. Cambiar el sistema de interrupciones es sensible, pero el scheduler sigue single-CPU.

Fase 3: Arrancar APs
Enviar INIT-SIPI-SIPI a cada AP.

Cada AP despierta, inicializa su entorno, y entra en ap_main.

Los APs hacen hlt en bucle esperando trabajo (aún no hay scheduler SMP).

Coste: 3-4 días. Riesgo: alto. Debuggear por qué un AP no arranca es tedioso.

Fase 4: Scheduler SMP
current_task se convierte en per_cpu(current_task, cpu).

Runqueues por CPU.

IPIs para resched remoto.

IPIs para TLB shootdown.

Locks reales en scheduler (runqueue locks).

preempt_disable por CPU.

Coste: 1-2 semanas. Riesgo: muy alto. Aquí es donde aparecen los bugs sutiles.

Fase 5: Auditar drivers y subsistemas
Compositor, winsrv, ps2, taskbar: revisar thread-safety.

IPC, wait queues, process: revisar.

Heap, SLAB: revisar (ya tienen locks, pero hay que verificar).

Coste: 1 semana. Riesgo: medio. La mayoría del código ya está protegido, solo hay que auditar.

Por qué esta división
Cada fase se puede revertir sin perder el trabajo de las anteriores. Si en la fase 3 los APs no arrancan, puedes seguir en la fase 2 con una sola CPU y el kernel sigue funcionando. Si en la fase 4 el scheduler peta, vuelves a la fase 3 con un solo CPU activo y sigues teniendo un kernel funcional.

Sin esta división, es fácil romper todo y pasar semanas sin un kernel que arranque.

Fase 0 — Inventario
Vamos a hacer un inventario sistemático. Necesito saber todo el código que asume:

Una sola CPU (variable global única donde debería haber per_cpu).

IRQs siempre disponibles (asumir que cli/sti son globales cuando con APIC no lo son del todo).

Estado global compartido sin locks.

IDs de tarea únicos globales (en SMP pueden chocar).

Código a auditar (por orden de impacto)
1. kernel/sched.c — crítico
current_task es global. Con SMP, cada CPU tiene su propia tarea actual.

task_list_head es global. Necesita un lock real (hoy no lo tiene).

next_id es global. Con SMP, dos CPUs pueden crear tareas concurrentemente y colisionar.

tick_counter es global. Debería ser per-CPU.

kernel_cr3 es global. Es correcto (el kernel es único).

preempt_count es per tarea, correcto.

El scheduler no tiene runqueues. task_list_head es una lista circular única. Con SMP necesitas N runqueues (una por CPU) o una con lock.

sched_tick hace cambios de tarea sin IPI a otras CPUs.

2. kernel/main.c — crítico
tick_count es global. Debería ser atómico o per-CPU.

cpu_local es global (una sola instancia). Debe ser per-CPU.

syscall_kernel_stack es global (un solo stack). Debe ser per-CPU.

3. kernel/pmm.c, buddy.c, slab.c, heap.c — importan
Tienen locks. Pero hay que verificar que son correctos para SMP (algunos usan spin_lock_irqsave que deshabilita IRQs del CPU actual, no globalmente).

used_blocks, last_alloc_bit, etc.: protegidos por el lock. Correcto.

slab_top: global con lock. Correcto.

4. kernel/gfx/* — importan
window_stack en compositor.c está protegido con preempt_disable, no con spinlock. Con SMP, preempt_disable no basta porque otras CPUs pueden tocar window_stack. Necesita spinlock.

taskbar_items en taskbar.c no tiene lock. Con SMP, race.

5. kernel/ipc.c, wait.c, vfs.c, process.c — importan
process_list con preempt_disable. Necesita spinlock real.

wq con spin_lock_irqsave. Correcto (deshabilita IRQs del CPU actual + lock global).

ipc_mailbox tiene un wq. Ya verificado.

6. kernel/tty.c, ps2.c — importan
tty_receive_char viene de IRQ. Con LAPIC, una IRQ puede llegar a cualquier CPU. El TTY puede correr en cualquier CPU. El lock protege.

g_console_win se toca sin lock. Con SMP, race.

7. kernel/idt.c, gdt.c, syscall_entry.asm — críticos
idt, gdt son compartidos. Correcto.

syscall_entry.asm usa [rel cpu_local + offset] para acceder al stack de kernel. Con SMP, cada CPU necesita su propio cpu_local. Hay que migrar a %gs:offset.

8. kernel/serial.c, klog.c — importantes
El lock de serial protege. Con SMP, más contención pero correcto.