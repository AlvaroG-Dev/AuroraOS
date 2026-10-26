# LinuxKPI: sesión del 26-oct-2026 (fase L2, K2)

Registro de la fase L2 de la capa de compatibilidad con drivers de Linux: sincronización, tiempo y trabajo diferido. El plan es `Plan_LinuxKPI.md` (decisiones con fecha en su §11); la sesión anterior, `docs/LinuxKPI_Sesion_L0_L1.md`.

Estado: **L2 terminada**. **L3 (modelo de dispositivos, PCI, IRQ) no empezada**: espera confirmación del autor.

---

## 1. Decisiones tomadas (con el autor, antes de escribir código)

| Tema | Decisión |
|---|---|
| RCU | **Contador de estados quiescentes por CPU en el tick** (opción A). |
| `mutex`/`wait_queue`/`workqueue`… | **Escritos en la capa** con la semántica de Linux, sobre un único servicio de Aurora: aparcar y despertar una tarea. |
| Cambios en el planificador | **Solo el gancho de RCU** en `sched_tick()`. Rechazados: plazos de espera a 1 ms, `task_t->lkpi_task` y exportar `signal_pending`. |

Detalle y consecuencias en `Plan_LinuxKPI.md` §11 («26-oct-2026 — L2»).

## 2. Qué se ha hecho

### Lado Aurora (`kernel/`)
- `sched.c`/`sched.h`: `sched_tick()` cuenta por CPU los ticks que llegan con `preempt_count == 0` y los expone con `sched_rcu_qs_count(cpu)`. Es el único cambio de comportamiento en Aurora.
- `ksymtab_exports.c`: exporta a los `.kur` primitivas que ya existían (`sched_yield`, `sched_set_blocked_deadline`, `wait_queue_init/add_locked/remove_locked`, `wake_up_all_locked`, `spin_lock_irqsave`, `kthread_create/start/stop/task`, `klog_get_tsc_freq`, `smp_online_cpus`, `preempt_count`). Un `_Static_assert` protege la réplica de `wait_queue_t` en `kernel/linuxkpi/aurora.h`.

### Cabeceras (`include/linuxkpi/`)
`spinlock`/`raw_spinlock`/`rwlock` completos (sustituyen al mínimo de L1), `lockdep` (no-op), `local_lock`, `cleanup.h` (`guard`, `scoped_guard`, `scoped_cond_guard`, `__free`), `preempt`, `bottom_half`, `sched.h` (`task_struct` mínimo, `current`, `schedule*`, `wake_up_process`, `set_current_state`), `wait.h` (`wait_queue_head`, `wait_event*`, `wake_up*`, `wait_woken`), `wait_bit.h`, `completion`, `mutex`, `ww_mutex`, `rwsem`, `semaphore`, `seqlock`, `rcupdate`/`rculist`/`srcu`, `refcount`/`kref`, `percpu`, `cpumask`, `smp`, `timer`, `hrtimer`, `delay`, `workqueue`, `interrupt.h` (tasklets), `irq_work`, `kthread`, `notifier`, y redirecciones (`sched/*`, `asm/current.h`, `hardirq.h`…). `ktime.h`: reloj con TSC.

### Implementación (`kernel/linuxkpi/kernel/`)
| Fichero | Contenido |
|---|---|
| `sched.c` | `task_struct` por tarea de Aurora (tabla hash `task_t* → task_struct`, lectura sin cerrojo); `schedule()`/`schedule_timeout()`/`wake_up_process()` sobre una `wait_queue_t` de Aurora por tarea; kthreads de Linux (dormidos hasta `wake_up_process`, `kthread_stop` con resultado); CPUs en línea, `percpu` dinámico. |
| `time.c` | `ktime_get()` con el TSC (ancla en un flanco de tick, monótono entre CPU), `udelay`/`ndelay`/`mdelay` activos, `msleep`, `usleep_range` (activo por debajo de 2 ms). |
| `wait.c` | Colas de espera (funciones de despertar, exclusivas, con clave), `completion`, colas por dirección (`wait_var_event`, `wait_on_bit`). |
| `locking.c` | `mutex` (propietario, traspaso FIFO, interrumpible), `ww_mutex` (wait-die y wound-wait, `-EDEADLK`/`-EALREADY`, `lock_slow`), `rwsem`, `semaphore`, `refcount`, notificadores. |
| `rcu.c` | Periodo de gracia por estados quiescentes, `call_rcu`/`kfree_rcu`/`rcu_barrier` por lotes en el hilo `lkpi_rcu`, `get/poll_state_synchronize_rcu`, SRCU básico. |
| `softirq.c` | Hilo `lkpi_softirqd`: `timer_list`, `hrtimer`, tasklets, `irq_work` (cada función con la expulsión desactivada). |
| `workqueue.c` | Workqueues de Linux sobre un grupo de hilos compartido que crece y encoge (`lkpi_wq/N` + gestor `lkpi_wq_mgr`). |

Además: `xa_node_free()` usa `kfree_rcu()` y `xa_load`/`xa_find` leen bajo `rcu_read_lock()` (pendiente de L1). `linuxkpi.kur` ≈ 250 KiB, 297 exports.

### Pruebas (`tests/linuxkpi/`)
- `lkpi_test_k2` (`TEST_FLAG_BLOCKING`): spinlocks/rwlock/seqlock/guards, refcount/kref, tiempo (resolución de `ktime`, `udelay`, `msleep`, `usleep_range`, `schedule_timeout`), kthreads, `wait_event*`, `completion`, `mutex` con 4 hilos, `ww_mutex` (wait-die en un hilo con `-EALREADY`, `-EDEADLK` y reintento; wound-wait con dos hilos), `rwsem`/`semaphore`, RCU (lector que bloquea `synchronize_rcu`, `call_rcu`/`kfree_rcu`/`rcu_barrier`, `rculist`, SRCU con lector que duerme), timers y hrtimers (rearmado, `del_timer_sync`, `timer_shutdown`, `hrtimer_forward`), workqueues (reencolado en curso, no reentrada, `flush_work`, `cancel_work_sync`, FIFO en ordenadas, `delayed_work`, `mod_delayed_work`, crecimiento del grupo con trabajos que duermen, `rcu_work`), tasklets, `irq_work`, `percpu`, `cpumask`, notificadores.
- `lkpi_test_k2_smp` (`TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP`), hito de L2, con `2 × CPUs` hilos por fase (8 con `-smp 4`):
  1. contención de `mutex` (3000 vueltas por hilo, durmiendo a veces con el mutex);
  2. `ww_mutex` wound-wait y 3. wait-die (8 cerrojos, 3 al azar en orden aleatorio, 400 rondas por hilo, protocolo de reintento);
  4. work + timers: 4 `timer_list` que se rearman y encolan trabajo (cola normal y ordenada), `mod_delayed_work`, un `hrtimer` periódico y productores en hilos; comprueba que cada `queue_work()` que devuelve `true` es exactamente una ejecución y que no hay reentrada;
  5. RCU: 7 lectores y un escritor que reemplaza el objeto y lo libera con `call_rcu` o `synchronize_rcu` (los lectores detectarían un objeto ya liberado);
  6. ping-pong de `completion` entre dos hilos (2000 vueltas, despertares perdidos).
  Cada fase tiene un tope de 120 s: un interbloqueo hace fallar la prueba en lugar de colgar la suite.

## 3. Fallos encontrados por el estrés

1. **Despertar perdido en wound-wait** (capa). La víctima comprobaba `wounded` antes de `set_current_state()`, y quien la hiere escribe y despierta desde el `wait_lock` de **otro** cerrojo. Las dos fases de `ww_mutex` se interbloqueaban con 4 CPU. Arreglo: fijar el estado al principio de cada vuelta, como Linux.
2. **`sched_current()` de Aurora no es atómico respecto a la expulsión** (Aurora; esquivado en la capa). `this_cpu(current_task)` se lee en dos pasos (`%gs:16` → `cpu_local_data[cpu].current_task`). Si la tarea migra entre ambos, obtiene la tarea actual de su CPU anterior. `preempt_disable()` incrementaba entonces el contador de **otra** tarea, que quedaba no expulsable; al terminar se quedaba en `kthread_do_exit` ocupando la CPU (`HLT` con `preempt_count > 0`, visto con el monitor de QEMU), y el periodo de gracia de RCU no avanzaba nunca en esa CPU. La capa hace esas lecturas (`current`, `preempt_count`, `preempt_disable`) con las IRQ desactivadas, y llama igual a `sched_yield()` y `kthread_stop()` de Aurora, que leen la tarea actual por dentro (`kthread_stop` → `wait_common` puede dejar BLOCKED a otra tarea si la lee mal). Los hilos propios de Aurora (escritorio, workers) siguen expuestos. **El arreglo en Aurora queda pendiente de decisión**: leer `current_task` con una sola instrucción relativa a `%gs` (`movq %gs:24, …`). El mismo patrón está en `sched_yield()`, `wait.c`, `kthread_should_stop()` y en los `preempt_disable()` del compositor y del *shootdown* de TLB.

## 4. Pruebas

Todo en QEMU TCG, `-m 6G`, un solo QEMU cada vez. La suite del kernel se esperó con `--done "passed, "` (ver §6).

| Prueba | Al empezar (sin cambios) | Al terminar |
|---|---|---|
| Suite del kernel (`-smp 2`) | 189/189 (16 omitidas) | **191/191** (16 omitidas; +K2 y +K2 SMP) |
| Suite del kernel (`-smp 4`) | — | **191/191** en 6 de 6 arranques con el código final |
| `drm_test`, `sig_test`, `rseq_test` | ALL PASS | ALL PASS |
| `clipboard-x11.sh`, `primary-icons.sh` | pasan | pasan (el `bash` de `clipboard-x11.sh` terminó bien; no se reprodujo el cuelgue de la sesión anterior) |

Estrés con 4 CPU (una ejecución típica, ≈ 9 s): 4100–5000 `-EDEADLK` con reintento en wound-wait y 1500–2200 en wait-die (3200 rondas cada una); 2000–4500 trabajos de work+timers con la cuenta exacta «`queue_work` → una ejecución»; 0,6–5 millones de lecturas RCU sin ver un objeto liberado; los hilos corren en las 4 CPU (máscara `0xf`).

Historia de las ejecuciones con 4 CPU antes del código final: la primera falló en las dos fases de `ww_mutex` (§3.1); con eso corregido, se atascó la fase de RCU (§3.2); tras corregir `current`/`preempt_disable`, 2 de 4 ejecuciones pasaron, otra se interbloqueó en wait-die y otra falló por una carrera de la propia prueba (`reinit_completion` después de `kthread_run`). Con `sched_yield`/`kthread_stop` de Aurora llamados con las IRQ desactivadas y la prueba corregida, 6 de 6 en verde. **El interbloqueo de wait-die no se reprodujo después y no se llegó a capturar su estado**: la explicación más probable es la lectura no atómica de `current` dentro de Aurora (§3.2), pero no está demostrado. El estrés vuelca ahora el estado de los hilos y de los cerrojos si vuelve a pasar.

Intermitencia ajena a la capa: en 1 de 6 arranques `iso9660: listado de /cdrom` se omite (17 omitidas en vez de 16).

## 5. Qué no se ha podido comprobar

- Ningún driver de Linux real usa todavía K2 (llegan en L3–L5); la API se ha medido con `docs/linuxkpi_api.md` (Linux 6.18.59 desde §9; NVIDIA aún 615.71.09 de Ubuntu).
- `signal_pending()` es siempre 0 (decisión: K8, L4): las esperas `_interruptible`/`_killable` no se han probado con señales reales.
- Resolución de los plazos: 1 ms desde §9, pero con todas las CPU ocupadas el despertado puede esperar a otra tarea `READY` (planificador round-robin sin prioridades).
- KVM y hardware real: todo en QEMU TCG. La deriva entre el TSC y el tick en ejecuciones largas no se ha medido.
- GitHub sigue bloqueado en el entorno (403 del proxy): NVIDIA open 615.78.08 no se ha descargado.

## 6. Contradicciones encontradas (planes frente al código)

- `Plan_LinuxKPI.md` §4 L2 y §1: «`workqueue`/`delayed_work` sobre `workqueue.c`». `kernel/workqueue.c` no tiene la semántica de Linux: `queue_work()` sobre un trabajo **en curso** devuelve `false` y se pierde (`workqueue.c:272`, CAS IDLE→QUEUED); hasta 16 hilos por cola (`:190`); los workers se despiertan cada 100 ticks (`:14`); faltan `mod_delayed_work`, `cancel_delayed_work` sin `_sync` y `work_pending`.
- Plazos: el plan y L1 suponen resolución de 1 ms (`HZ = 1000`), pero `sched_wake_expired()` corre cada 8 ticks (`kernel/time.c:84`).
- `kernel/completion.h:54` dice que las esperas pueden devolver `-EINTR`, pero `completion.c:49` usa `wait_event` (no interrumpible). Además, comprobar `done` y decrementarlo no es atómico (`completion.c:49-52`): dos waiters con `done == 1` pueden salir los dos.
- `kernel/wait.c`: la condición es un callback `bool (*)(void *)`. No sirve para `wait_event(wq, expresión)` de Linux, y no hay entradas con función de despertar propia ni waiters exclusivos.
- `kernel/sched.c`: `sched_yield()` no cambia de tarea con `preempt_count > 0` (`:444`). Además, `ticks_since_resched` se pone a 0 antes de mirar `preempt_count`: una tarea que pasa casi todo el tiempo con la expulsión desactivada solo se puede expulsar cada 10 ticks.
- `scripts/qemu-drive.py` fija `-smp 2` (`:44`); `--extra=-smp --extra=4` funciona porque QEMU se queda con el último valor. Con `--boot-test` el script puede terminar antes de que acabe la suite del kernel (corre a la vez que el userland): para la suite, `--done "passed, "`.

## 7. Pendiente y decisiones abiertas

1. ~~Arreglar `sched_current()` en Aurora~~ → hecho (§9).
2. ~~Plazos a 1 ms~~ → hecho, con expulsión acotada a los despertares por plazo (§9).
3. **`current` estable** (`task_t->lkpi_task` + gancho al liberar) y **`signal_pending`**: siguen rechazados; necesarios en K8 (L4).
4. ~~`disable_work_sync`, percpu estático, SRCU global~~ → hecho (§9). Queda `this_cpu_inc(p->campo)` sobre structs por CPU y `on_each_cpu()` (IPI de función, L3/L7).
5. De L0–L1: NVIDIA 615.78.08 (GitHub bloqueado) y la decisión NVIDIA; módulos > 64 MiB (L8).
6. **L3**: espera confirmación.

## 8. Commits de la sesión

| Commit | Contenido |
|---|---|
| `95d0c226` | Aurora: gancho de RCU en `sched_tick` y exports para K2 |
| `1a1f92b5` | Cabeceras K2 |
| `35dfe9ae` | Implementación de K2 (`kernel/linuxkpi/kernel/`), `xarray` con `kfree_rcu` |
| `cf4d8481` | Pruebas `lkpi_test_k2` y `lkpi_test_k2_smp` |
| `8c3e689e` | `ww_mutex`: estado antes de comprobar (despertar perdido en wound-wait) |
| `42b32f62` | `current`/`preempt_count`/`preempt_disable` de Aurora sin expulsión |
| `2656d44b` | `sched_yield`/`kthread_stop` de Aurora con las IRQ desactivadas; arreglo de la prueba |
| `6a40683d` | `preempt.h` incluye `irqflags.h`; `alloc_percpu` con `kvzalloc` |
| `7174c65f` | Plan §11 y este registro |

## 9. Después de L2 (mismo día, con confirmación del autor)

- **Versión fijada**: Linux **6.18.59** y `linux-firmware` **20261019** (`kernel/linuxkpi/linux-version.mk`); `scripts/fetch-linux-drivers.sh --no-nvidia` la descarga de kernel.org. **L0.3 repetida** sobre la 6.18.59 (`docs/linuxkpi_api.md`/`.tsv`, resumen en `Plan_LinuxKPI.md` §3.1): primer hito en QEMU 694 símbolos (antes 634), total 6104 (antes 5717), K2 452; NVIDIA 59 % compartido con Intel/AMD. NVIDIA sigue siendo la 615.71.09 de Ubuntu (GitHub, 403).
- **`sched_current()` atómico** en Aurora (`this_cpu_current_task()`, un `movq %gs:24`), usado también en `sched_tick()`. La capa ya no necesita llamar con las IRQ desactivadas.
- **Plazos a 1 ms**: `sched_wake_expired()` también en el tick del plazo más cercano y, si el despertado no encuentra CPU ociosa, `need_resched` en la CPU cuya tarea lleva más tiempo seguida. Prueba del kernel que mide el instante del despertar (con el código anterior, 1 de 12 a tiempo).
- **Capa**: `disable_work`/`enable_work` reales, SRCU por CPU, percpu estático por alias (valen `this_cpu_ptr(&v)` y `per_cpu_ptr(&v, cpu)`), `LKPI_USLEEP_SPIN_US` 1 ms.

**Regresión tras estos cambios**: suite del kernel 192/192 con `-smp 2` y 5 de 5 con `-smp 4` (plazos 1 ms: 12 de 12 en todas); `drm_test`, `sig_test`, `rseq_test` ALL PASS; `clipboard-x11.sh` pasa. `primary-icons.sh` se colgó una vez (bash esperando a `wl-copy`, que no terminaba tras lanzar `cat` y `xdg-mime`); repetido 4 veces, pasó las 4. Coincide con la intermitencia ya conocida (~5 de 45 antes de estos cambios), así que no lo atribuyo a ellos, pero sigue abierto.

