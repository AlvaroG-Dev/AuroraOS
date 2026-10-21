Plan de implementación: sistema de drivers .aur / .kur
Tras leer driver.c/h, main.c, process.c, sched.c, syscall.c, wait.c, completion.c, vfs.c, sysfs.c, elf.c, idt.c, spinlock.c y tus respuestas, tengo claro el punto de partida. Resumen ejecutivo antes del plan:

Lo que ya está bien y vamos a reutilizar:

sched_create_task crea kernel threads de verdad; task_t ya tiene waiting_on, wait_entry, wake_deadline.

wait_queue_t con wait_event_interruptible_timeout, suscriptores (para epoll), y wake_timeout_task.

completion_t con variante uninterruptible (perfecto para I/O en vuelo).

epoll, signalfd, eventfd, futex, memfd, sockets con sendmsg/recvmsg.

elf_load_streaming con PT_INTERP, AT_PHDR, vDSO.

mmio_map (usado en fb_init), paging_map_page_in, uaccess.

sysfs con /sys/bus/pci/devices/<BDF>/{config,resource,...} — punto de entrada natural para mmap de BARs.

block.c con blk_read/blk_write síncronos y drivers AHCI/ATA-PIO/ATAPI ya funcionando.

netif.c + e1000e.c con kthread RX.

Lo que falta para "no bloquear":

drivers_init_all() recorre drivers secuencialmente en kmain_task y bloquea el boot.

No hay workqueues ni kthreads genéricos con nombre ni IRQ threading.

blk_read/blk_write son síncronos — cada read() de usuario serializa el disco.

netif_xmit (y el envío en e1000e) es síncrono.

AML corre inline en boot.

No existe .aur ni .kur, ni loader, ni tabla de símbolos exportados.

No hay capacidades: cualquier proceso puede mmap de sysfs//dev/* sin chequeo.

Decisiones de arquitectura (para "lo que tú veas")
Decisión	Elección	Motivo
Modelo async	io_uring-style con aur_req_t kernel-side y anillos SQE/CQE user-side	Es lo más profesional, encaja con epoll/completion que ya tienes, y da cancelación, timeouts, cero copias.
Formato kernel	.kur = ELF ET_REL + manifiesto struct kur_manifest + ksymtab	Como .ko pero autocontenido en un solo segmento.
Formato user	.aur = ELF ET_DYN + manifiesto struct aur_manifest + capabilities	Cargable con elf_load_streaming tal cual.
Frontera kernel/user	gradual: kernel primero, migración por fases cuando el ABI esté probado	Tú lo pediste; reduce riesgo.
MMIO a user	mmap() sobre /sys/bus/pci/devices/<BDF>/resource<N> (Linux-style)	sysfs ya está; solo hay que añadir el ops mmap.
IRQ a user	/dev/irq/<N> como char device con read() (contador) y epoll	Reutiliza tu infra de epoll y wake_up_all.
DMA a user	/dev/dma con DMA_ALLOC devuelve fd backed por memfd/pool; DMA_MAP para pin de páginas user	Sin IOMMU; bounce buffers en kernel + verificación de ownership.
Capacidades	Bitmap por proceso + lista de device_ownership por .aur	Sin capabilities de Linux, chequeo simple en sys_mmap/ioctl.
Prioridad	Primero infra de no-bloqueo, después ABI, después migrar drivers	Sin workqueues, ningún .aur puede ser async de verdad.
PLAN COMPLETO
FASE 0 — Preparación (1–2 días)
0.1 Inventario de puntos de bloqueo
Files: kernel/DISK_ROADMAP.md (nuevo AUR_ROADMAP.md)

Auditar cada llamada bloqueante del kernel (blk_read, wait_for_completion, sched_sleep_ticks, tty_read).

Marcar cada una con [BLOCK] en el código con un comentario grep-friendly.

Acceptance: grep -rn '\[BLOCK\]' kernel/ | wc -l da un número finito y manejable (<100).

0.2 Congelar sched.h y wait.h (contrato base)
Files: kernel/sched.h, kernel/wait.h

Añadir a task_t: char name[16] (nombre de kthread), int kthread_should_stop, uint64_t kthread_exit_code.

No tocar nada más (respeta _Static_assert de offset — el name[] va al final del struct o dentro del padding de fpu_raw que no esté cubierto).

Acceptance: boot completo + tests actuales pasan; los asserts siguen cuadrando.

FASE 1 — Infra kernel de no-bloqueo (la base de todo)
Esta fase no introduce .aur ni .kur. Sin esto, cualquier intento de asincronía se queda en cosmético. Duración estimada: 2–3 semanas.

1.1 — Workqueues
Depends: 0.2
Files: kernel/workqueue.c (nuevo), kernel/workqueue.h (nuevo), kernel/main.c, kernel/sched.c

Tasks:

struct work_struct { void (*fn)(struct work_struct*); void *data; atomic_t pending; struct work_struct *next; }.

INIT_WORK, schedule_work, schedule_delayed_work(d, ticks), flush_work, cancel_work_sync, queue_work_on(cpu, w).

Dos colas globales: system_wq (per-CPU pool de NR_CPUS kworkers) y system_ordered_wq (un solo kworker, para AML y cosas que no pueden correr en paralelo).

Pool creado en smp_boot_aps() (ya tienes hook).

Usa wait_event_interruptible_timeout para dormir el kworker y wake_up_one para despertarlo.

Acceptance:

Test wq_stress en test.c: 10.000 schedule_work desde 4 CPUs, todos ejecutados, sin deadlock, flush_work retorna.

Test wq_ordered: dos works ordenados por delayed_work se ejecutan en el orden programado.

Boot mide <5% de penalización por crear kworkers.

1.2 — Kthread API con nombre
Depends: 1.1
Files: kernel/kthread.c (nuevo), kernel/kthread.h (nuevo)

Tasks:

kthread_create(fn, arg, fmt, ...) devuelve task_t* en TASK_READY (sin publicar).

kthread_run(...) = create + sched_publish_task.

kthread_stop(t): setea t->kthread_should_stop = 1, wake_up_one(t->waiting_on), espera a TASK_DEAD con wait_event_timeout.

kthread_should_stop(), kthread_data(), kthread_exit(code).

En task_entry_wrapper (en sched.c): si t->kthread_should_stop, salir limpio.

Acceptance:

kthread_create de un loop que cuenta; kthread_stop lo para y devuelve el código.

ps (o cat /proc/<pid>/comm) muestra el nombre del kthread.

1.3 — Threaded IRQs
Depends: 1.2
Files: kernel/idt.c, kernel/idt.h

Tasks:

Nueva API: int irq_request_threaded(unsigned irq, irq_handler_t ack, irq_handler_t thread_fn, const char *name, void *dev_id, unsigned long flags).

ack corre en contexto duro, rápido, sin locks ni allocs; devuelve IRQ_HANDLED o IRQ_WAKE_THREAD.

thread_fn corre en un kthread dedicado al IRQ (creado en el primer request).

irq_free(irq) sincroniza y libera el kthread.

Mantener irq_install_handler y irq_install_handler_ex como legacy (migrar callsites después).

Acceptance:

e1000e migra su IRQ a threaded (ack lee ISR y limpia; thread_fn procesa RX/TX).

ping 10.0.2.2 con -f sube 3× el throughput vs. la versión con todo en hard-IRQ.

Nada en ack falla si el thread_fn no está corriendo (IRQ safe).

1.4 — Async block I/O
Depends: 1.1, 1.3
Files: kernel/block.h, kernel/block.c, kernel/ahci.c, kernel/ata_pio.c, kernel/ata_dma.c, kernel/atapi.c

Tasks:

struct bio { block_device_t *bdev; uint64_t lba; uint32_t nsec; int rw; void *buf; completion_t done; int status; }.

blk_submit_bio(bio) → enruta a bdev->ops->submit y retorna.

blk_endio(bio, status) → complete(&bio->done) (llamable desde IRQ).

blk_read/blk_write quedan como wrappers: alloc bio en stack, submit, wait_for_completion_uninterruptible, return.

blk_ops_t.submit(bdev, bio) — nuevo op en cada driver.

AHCI: ahci_submit rellena el command slot, arranca el port, retorna; IRQ del port hace blk_endio.

Acceptance:

dd if=/dev/hda of=/dev/null bs=1M count=100 sube de ~X MB/s a ~3X MB/s (medir antes/después).

blk_submit_bio desde 4 CPUs en paralelo, todos completan; bio->status consistente.

fsync sigue funcionando.

1.5 — Async netif TX
Depends: 1.3
Files: kernel/net/netif.h, kernel/net/netif.c, kernel/net/e1000e.c

Tasks:

struct netif_ops { int (*xmit)(netif_t*, struct sk_buff*, completion_t*); }.

netif_xmit_async(netif, skb, completion_t*).

netif_xmit queda como wrapper bloqueante (para el stack IP actual).

e1000e: e1000e_xmit llena el TX ring descriptor, mueve tdt, retorna; IRQ de TX completa.

Acceptance:

ping -f 10.0.2.2 sin packet loss.

iperf (si lo portas) con 4 conexiones paralelas no degrada.

1.6 — AML como trabajo diferido
Depends: 1.1
Files: kernel/acpi_aml.c, kernel/acpi.c

Tasks:

aml_lock global (mutex) para serializar el intérprete (AML no es reentrante).

aml_evaluate_async(method, args, cb) → schedule_work(system_ordered_wq, ...).

Las llamadas de boot (_STA, _INI, _CRS) que hoy son inline → async con wait_for_completion explícito donde sea necesario, o fire-and-forget.

acpi_pm.c (P-states, S-states) → igual.

Acceptance:

acpidump y acpixtract funcionan.

cat /proc/acpi/* responde sin bloquear el BSP.

El boot es perceptiblemente más rápido.

1.7 — Per-CPU runqueues del scheduler
Depends: 1.1
Files: kernel/sched.c, kernel/sched.h
Importancia: media. La lista global con sched_lock no es el cuello de botella hoy, pero lo será cuando muchos .aur corran. Lo pongo en fase 1 como opcional.

Tasks:

struct rq { task_t *head; spinlock_t lock; uint64_t ticks_since_resched; }.

sched_tick opera sobre this_cpu(rq).

Load balance simple: cada N ticks, si una CPU está idle y otra tiene ≥3 tareas, migra una.

Acceptance:

stress-ng --cpu 8 escala a >80% del ideal en 8 CPUs.

sched_yield no produce migraciones espurias.

FASE 2 — Interfaz de driver v2 (kernel y user)
2.1 — driver.h v2 (drivers de kernel, .kur)
Depends: 1.1, 1.2
Files: kernel/driver.h, kernel/driver.c, kernel/bus.c (nuevo), kernel/bus.h (nuevo), kernel/main.c

Tasks:

struct device { const char *name; struct bus_type *bus; void *priv; struct device_id *id; ... }.

struct bus_type { const char *name; int (*match)(dev, drv); int (*probe)(dev); int (*remove)(dev); }.

Buses a implementar: pci_bus, usb_bus, platform_bus, virtio_bus (stub).

struct driver { const char *name; const struct device_id *id_table; int (*probe)(struct device*); void (*remove)(struct device*); const struct driver_ops *ops; ... }.

driver_register(d) → bus_add_driver.

device_register(dev) → matchea contra drivers existentes y encola probe en system_wq (¡no lo ejecuta inline!).

Eliminar drivers_init_all() de kmain_task; reemplazar por drivers_probe_all() que arranca los probes async y retorna; drivers_probe_wait() bloquea hasta que todos terminen (o timeout).

drivers_shutdown_all() → driver_unregister inverso.

Acceptance:

Los 6 drivers actuales (ps2, pci, ata_pio, atapi, ahci, xhci) migran a driver_register + .probe.

Boot time baja de T_total a T_total - Σprobe_secuenciales, medible con klog_get_tsc_freq.

ls /sys/bus/pci/drivers/ muestra los drivers registrados (sysfs extendido).

Hot-plug de un device virtual de QEMU (-device edu) → probe ejecutado y driver cargado.

2.2 — aur.h: modelo asíncrono universal
Depends: 2.1
Files: kernel/aur.h (nuevo), kernel/aur.c (nuevo)

Tasks:

struct aur_req { completion_t done; int64_t result; uint64_t cookie; struct aur_req *next; atomic_t refs; }.

struct aur_ring { aur_req_t *sq_head, *sq_tail; aur_req_t *cq_head, *cq_tail; wait_queue_t cq_wq; }.

aur_req_alloc(), aur_req_submit(dev, req, ops, ...), aur_req_wait(req), aur_req_cancel(req).

aur_dev_t = struct device + aur_ring_t.

Regla: cualquier driver nuevo declara const aur_ops_t *ops en struct driver. Las ops pueden ser síncronas (marcadas con AUR_OP_SYNC en el manifiesto), pero el default es async.

Acceptance:

Test: submitea 1000 reqs a un driver de prueba que hace schedule_delayed_work, todas completan en <1s.

Cancelación: 100 reqs canceladas antes de completar, result == -ECANCELED.

2.3 — Colas por dispositivo y sincronización
Depends: 2.2
Files: kernel/aur.c, kernel/block.c, kernel/net/netif.c

Tasks:

Cada aur_dev_t tiene una cola (o varias si AUR_RING_PER_CPU en el manifiesto).

aur_submit encola al sq, wake_up_one(&dev->wq).

Driver (kernel o .aur) consume sq, procesa, aur_complete(dev, req, status) → encola a cq y wake_up_all(&cq_wq).

Shutdown: aur_ring_drain(dev, timeout).

Acceptance:

4 lectores concurrentes de /dev/hda no se bloquean entre sí (medir latencia P99 < 10ms).

FASE 3 — .kur (kernel modules)
3.1 — Formato .kur + manifiesto
Depends: 2.1
Files: kernel/kur.h (nuevo), tools/kurbuild.c (nuevo)

Tasks:

ELF ET_REL con secciones .text, .rodata, .data, .bss (estándar).

Sección extra .kur con struct kur_manifest serializado (JSON-like o binario compacto — recomiendo binario con magic/version):

c
struct kur_manifest {
  uint32_t magic;         // 0x4B555221 "KUR!"
  uint16_t version;       // KUR_ABI_VERSION
  uint16_t name_len;
  char     name[];        // "ahci_kur"
  uint32_t deps_count;
  dep_t    deps[];        // {name, version_min}
  uint32_t ksym_count;
  ksym_t   ksyms[];       // {name, is_export}
};
kurbuild: gcc -c -fno-pic -mcmodel=kernel ... && ld -r -o ahci.kur ahci.o && objcopy --add-section .kur=....

Validación: tamaño total <1MB (o límite elegido).

Acceptance:

readelf -S ahci.kur muestra .kur.

kurbuild produce .kur válido y reproducible (byte-idéntico con mismo input).

3.2 — ksymtab y reubicaciones
Depends: 3.1
Files: kernel/ksymtab.c (nuevo), kernel/ksymtab.h (nuevo), kernel/kur_loader.c (nuevo)

Tasks:

KEXPORT_SYMBOL(fn) en ksymtab.c: sección .ksymtab con { name, addr }.

Al boot, ksymtab_init() ordena la tabla.

kur_loader_load(path):

Lee ELF ET_REL.
Valida secciones y manifiesto.
vmalloc un bloque contiguo para .text/.rodata/.data (nuevo allocador kmodule_alloc en paging.c, o mmio_map-like con NX).
Resuelve R_X86_64_PLT32, R_X86_64_PC32, R_X86_64_32S, R_X86_64_64 contra ksymtab.
mprotect .text RO+X, .rodata RO.
Llama init().
kur_loader_unload(name): llama exit(), vfree, desregistra del bus.

Acceptance:

Cargar test_kur.ko que hace LOG_INFO y exporta una función. Otro módulo que importa esa función también carga.

R_X86_64_PC32 a símbolo no exportado → -ENOENT con mensaje claro.

3.3 — Integración con initrd y bus
Depends: 3.2
Files: kernel/initrd.c (nuevo), kernel/main.c

Tasks:

/boot/modules.list en initrd lista los .kur a cargar, o el loader escanea /lib/modules/$(uname -r)/.

Tras vfs_init(), kur_load_all() carga cada .kur y los registra en bus_type.

Un .kur puede declarar en su manifiesto buses = ["pci"] y su .probe matchea automáticamente.

Acceptance:

Arrancar QEMU con un .kur que sustituya a ahci in-kernel; AHCI funciona idéntico.

rmmod ahci_kur desmonta la partición, modprobe ahci_kur la remonta.

FASE 4 — .aur (user drivers)
4.1 — Formato .aur + manifiesto
Depends: 2.2, 4.2
Files: kernel/aur_user.h (nuevo), tools/aurbuild.c (nuevo)

Tasks:

ELF ET_DYN + sección .aur con struct aur_manifest:

c
struct aur_manifest {
  uint32_t magic;         // 0x41555221 "AUR!"
  uint16_t version;
  uint16_t name_len;
  char     name[];
  uint32_t caps;          // AUR_CAP_MMIO | AUR_CAP_DMA | AUR_CAP_IRQ | ...
  uint32_t bus_match_count;
  struct { uint16_t vendor, device, class, subclass; } bus_match[];
  uint32_t devfd_count;   // dispositivos que expone vía /dev/<name>/
  char     devfds[][32];
  uint32_t ipc_services_count;
  char     ipc_services[][32];
  uint32_t ring_per_cpu;
};
aurbuild: gcc -pie -o e1000e.aur e1000e.c && objcopy --add-section .aur=....

Validación de manifiesto en aur_loader.c (nombre no vacío, caps coherentes, etc.).

Acceptance:

aurbuild produce .aur ejecutable standalone (en un Linux normal, es un ELF válido).

aurinfo e1000e.aur imprime caps, bus_match, servicios.

4.2 — Carga de .aur y caps por proceso
Depends: 4.1
Files: kernel/aur_user.c (nuevo), kernel/process.c, kernel/process.h

Tasks:

process_t añade uint64_t aur_caps; uint32_t aur_dev_count; aur_dev_ref_t aur_devs[]; char aur_name[32]; int is_aur_driver;.

aur_load(path):

Lee manifiesto.
process_load normal.
Instala caps en proc->aur_caps.
Si bus_match no vacío, registra en pci_bus/usb_bus con un driver proxy que hace wake_up cuando matchea.
Pre-crea /dev/aur/<name>/ y sus dispositivos declarados.
sys_mmap extendido: si path es /sys/bus/pci/devices/<BDF>/resource<N> y proc->is_aur_driver, chequea CAP_MMIO y aur_dev_owns(proc, BDF).

Acceptance:

Cargar un .aur vacío con aur_load("/lib/aur/dummy.aur"), verificar ps con nombre, cat /proc/<pid>/cmdline.

Sin cap: -EPERM. Con cap: OK.

4.3 — Primitivas: MMIO
Depends: 4.2
Files: kernel/sysfs.c, kernel/paging.c

Tasks:

Añadir vfs_ops.mmap(node, vma) (nuevo op, retrocompatible: NULL = sin mmap).

sysfs resource<N> implementa .mmap que llama mmio_map(bar_base, size, flags) y mapea en proc->pml4_phys.

mmap en sys_mmap detecta resource<N> y enruta.

Acceptance:

Proceso user con CAP_MMIO abre /sys/bus/pci/devices/0000:00:03.0/resource0, hace mmap, lee el registro de vendor del e1000e. Byte exacto coincide con lspci -x.

4.4 — Primitivas: DMA
Depends: 4.2
Files: kernel/dma_user.c (nuevo), kernel/dma_user.h (nuevo)

Tasks:

/dev/dma con ioctls:

DMA_ALLOC(size, align) → devuelve fd (memfd-like) + phys addr en un struct dma_desc.

DMA_FREE(fd).

DMA_MAP(fd, offset, len) → phys (pins).

DMA_SYNC(fd, offset, len, dir).

El descriptor fd es un memfd ya existente (reutiliza memfd.c).

Acceptance:

dma_alloc(4096, 4096), escribir patrón, DMA_MAP, DMA del device escribe, leer de vuelta, patrón modificado coincide.

4.5 — Primitivas: IRQ
Depends: 1.3, 4.2
Files: kernel/irq_user.c (nuevo)

Tasks:

/dev/irq/<N> char device. read() devuelve uint64_t count. poll() retorna POLLIN si count > last.

epoll_ctl funciona sobre él.

ioctl(IRQ_REQUEST, {vector, flags}) → irq_request_threaded con handler que hace atomic_inc(&irq_user->count); wake_up_all(&irq_user->wq). IRQ_HANDLED.

IRQ_FREE → irq_free.

Solo accesible con CAP_IRQ y solo para IRQs que el manifiesto del .aur declare.

Acceptance:

Proceso abre /dev/irq/11, hace epoll_wait, genera tráfico en e1000e, recibe eventos.

4.6 — Primitivas: control channel (VFS node)
Depends: 4.2
Files: kernel/aur_user.c

Tasks:

Cada .aur declara en el manifiesto devfds[]. Al cargar, se crea /dev/aur/<driver>/<devfd>.

El driver escribe en /dev/aur/<driver>/<devfd>/ready para publicar el fd.

Los consumidores (kernel o user) abren el fd y hablan por ioctl/read/write.

Para net: el driver publica /dev/aur/e1000e/0 que el netif shim del kernel usa.

Acceptance:

.aur de prueba crea un fd, kernel lo abre y escribe, el driver responde.

4.7 — Bus matching user
Depends: 4.4, 4.5, 4.6
Files: kernel/bus.c

Tasks:

bus_match_async(dev) recorre .aur cargados y los que matchean reciben IPC con el BDF y los fds pre-mapeados.

Política: .kur tiene prioridad si también matchea (configurable).

Acceptance:

QEMU -device e1000e, .aur de e1000e es lanzado y matchea. lspci -k (portado) muestra "Kernel driver in use: e1000e.aur".

FASE 5 — Migración progresiva de drivers
5.1 — Bloque: kernel wrappers sobre aur_req
Depends: 4.1–4.7
Files: kernel/block.c

Tasks:

blk_submit_bio acepta aur_req y encola en la cola del aur_dev_t del disco.

blk_read/blk_write mantienen la API síncrona.

blk_mq opcional (multiqueue) si el driver lo pide.

Acceptance:

mkfs.ext2 en /dev/hda1 desde user funciona.

dd if=/dev/hda of=/dev/null bs=1M count=1000 a velocidad raw.

5.2 — ahci.aur
Depends: 5.1
Files: user/ahci.aur.c

Tasks:

Extraer la lógica de kernel/ahci.c a user.

Incluye: MMIO de BAR5, DMA rings (alloc con /dev/dma), IRQ (con /dev/irq/<N>), protocolo ATA.

Carga desde /lib/aur/ahci.aur.

Acceptance:

SHA256 de dd bit-idéntico al driver kernel.

Benchmarks: 95% del throughput kernel.

kill -9 $(pidof ahci.aur) → I/O falla con -EIO, restart en <2s.

5.3 — e1000e.aur
Depends: 5.2 (valida ABI)
Files: user/e1000e.aur.c

Tasks:

MMIO BAR0, RX/TX rings, DMA, IRQ.

Expone /dev/aur/e1000e/0 que el kernel netif_shim usa para xmit/rx.

dhcp, ping, wget deben funcionar sin tocar netif.c.

Acceptance:

ping 10.0.2.2 OK.

wget http://10.0.2.2:8080/test OK.

CPU <5% idle con tráfico a 100Mbps.

5.4 — USB split (kernel core / user xhci)
Depends: 5.3
Files: kernel/usb/ (split), user/xhci.aur.c, user/usb_hid.aur.c

Tasks:

usb_core.c queda en kernel (URB scheduling, device model, HID parser).

xhci.c se parte: ring management, TRBs → user. usb_core se comunica por /dev/aur/xhci/0.

usb_hid → user service que consume input events del kernel.

Acceptance:

Teclado y ratón funcionan.

lsusb (portado) enumera.

5.5 — Input completo user
Depends: 5.4
Files: user/ps2.aur.c (o usb_hid.aur.c)

Tasks:

El kernel input.c recibe eventos del driver user vía IPC.

Acceptance:

Teclado por PS/2 funciona.

Teclado y ratón por USB funcionan.

5.6 — GFX / winsrv como .aur service
Depends: 4.6
Files: user/winsrv.aur.c (ya casi)

Tasks:

winsrv.c actual es casi un service; formalizar como .aur con manifiesto ipc_services = ["winsrv"].

compositor y taskbar siguen siendo .aur.

Acceptance:

Escritorio completo funciona.

FASE 6 — Resiliencia y hot-plug
6.1 — Timeouts por op
Depends: 2.2
Files: kernel/aur.c

Tasks:

Cada aur_req tiene deadline_ticks = now + driver->timeout_ms.

sched_wake_expired extendido: si una req expira, marca result = -ETIMEDOUT, complete.

Driver puede sobreescribir timeout por op (AUR_OP_SLOW).

Acceptance:

Test de req sin completion: expira a los timeout_ms.

6.2 — Watchdog por driver
Depends: 6.1
Files: kernel/aur_user.c

Tasks:

Cada .aur tiene un kthread watchdog que verifica last_heartbeat cada N ms.

Si no responde, SIGKILL.

El driver debe hacer AUR_HEARTBEAT en cada loop o cada N ops.

Acceptance:

Un .aur que entra en while(1); es matado y reiniciado en <1s.

6.3 — Driver restart
Depends: 6.2
Files: kernel/aur_user.c

Tasks:

aur_restart(driver): SIGKILL, wait, aur_load again, rebind bus, replay estado (flush bios con -EIO).

Contador de restarts por driver; si >5 en 1 min → no reintentar, LOG_ERR.

Acceptance:

kill -9 $(pidof ahci.aur) → fds abiertos devuelven -EIO, /dev/hda1 se remonta solo en <3s.

6.4 — Hot-plug PCI
Depends: 6.3
Files: kernel/bus.c, kernel/pci.c

Tasks:

pci_scan_full() (no solo bus 0) en background al boot.

IRQ de hot-plug (PCIe native) → device_register → bus_match_async.

Acceptance:

Añadir -device edu a QEMU en runtime (si soportado) o simular.

FASE 7 — Tooling y tests
7.1 — aurbuild / kurmodprobe / aurinfo
Depends: 3.1, 4.1
Files: tools/

Tasks:

CLI que construye, valida, firma (opcional) y empaqueta.

aurinfo imprime manifiesto.

7.2 — Matriz QEMU
Depends: 5.x
Files: tests/qemu_matrix.sh

Tasks:

Arranca QEMU con distintas combinaciones: ahci / ata-pio / ata-dma, e1000e / virtio-net, xhci, acpi.

Corre tests funcionales (boot, dd, ping, lsusb).

Compara resultados vs. .kur in-kernel.

7.3 — Fuzzing de manifiestos y ELF
Depends: 7.1
Files: tests/fuzz_*.c

Tasks:

Fuzz .aur / .kur con cabeceras truncadas, manifiestos corruptos, relocations raras.

Esperado: -EINVAL/-ENOEXEC, nunca panic.

7.4 — Suite de stress
Depends: 5.x
Files: tests/stress_*.c

Tasks:

wq_stress, blk_parallel, net_flood, aur_restart_loop.

Todo integrado en run_all_tests().

FASE 8 — Extras (cuando aplique)
IOMMU/VT-d para DMA user-safe (fase 8, después de estabilidad).

Firma criptográfica de .aur/.kur.

Namespaces: aislar drivers en cgroups (fase futura).

Multiqueue blk (blk-mq) si el throughput lo pide.

virtio.aur para paravirtualización.