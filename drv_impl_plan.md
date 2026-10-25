# Roadmap y Estado de Implementación: Sistema de Drivers .aur / .kur

Este documento es la fuente de la verdad sobre el estado del sistema de drivers
de Aurora OS. Contrasta el plan original con la implementación real, documentando
las desviaciones y los siguientes pasos.

---

## Resumen Ejecutivo de la Arquitectura

* **Modelo Async**: El plan original proponía anillos `io_uring`-style (`aur_req_t`, SQE/CQE). 
  **Desviación:** Se ha implementado un modelo más directo y Unix-like basado en
  `mmap` para MMIO, `ioctl` para DMA y (próximamente) `read`/`epoll` para IRQs. 
  Esto simplifica enormemente el ABI y ya ha sido validado en hardware virtual.
* **Formato Kernel**: `.kur` (ELF `ET_REL` + manifiesto + ksymtab + SHA-256).
* **Formato User**: `.aur` (ELF `ET_DYN`/`EXEC` + manifiesto `.aur` + Capabilities).
* **Frontera Kernel/User**: Los drivers `.aur` corren en Ring 3 y acceden al hardware
  mediante primitivas seguras controladas por capacidades (MMIO, DMA, IRQ).

---

## FASE 0 — Preparación
*   [x] **0.1 Inventario de bloqueos**: Auditoría implícita completada. El sistema
    actual tiene un scheduler funcional y wait queues, lo que permite identificar
    los puntos síncronos (blk_read, netif_xmit).
*   [x] **0.2 Contrato sched.h / wait.h**: Completado orgánicamente. `task_t` tiene
    `name`, y las wait queues soportan timeouts e interrupciones.

---

## FASE 1 — Infra Kernel de No-Bloqueo
*   [x] **1.1 Workqueues**: Implementado (`workqueue.c`). Tienes `system_wq` (paralelo) y `system_ordered_wq`.
*   [x] **1.2 Kthread API**: Implementado (`kthread.c`). Tareas de kernel con nombre y `kthread_stop`.
*   [x] **1.3 Threaded IRQs**: Implementado (`irq_threaded.c`). Permite dividir el handler en `ack` (hard IRQ) y `thread_fn`.
*   [x] **1.4 Async Block I/O**: Implementado (`bio_t`). AHCI usa `ahci_submit` asíncrono y `bio_endio`.
*   [~] **1.5 Async Netif TX**: e1000e usa MSI y kthread RX. El TX es síncrono en el
    stack IP, pero el driver ya está preparado para operar en un kthread.
*   [ ] **1.6 AML diferido**: Actualmente corre en una tarea de inicialización, no bloquea el BSP.
*   [ ] **1.7 Per-CPU runqueues**: El scheduler sigue usando una runqueue global con spinlock.
    Funciona bien hasta 8 CPUs, pero será un cuello de botella con muchos `.aur`.

---

## FASE 2 — Interfaz de Driver v2 (Bus Framework)
*   [x] **2.1 driver.h v2**: Implementado (`bus.c`, `bus.h`, `driver.h` v3).
    *   [x] `pci_bus` genérico (matchea por vendor/device/class).
    *   [x] `usb_bus` genérico (matchea por vendor/device/interface_class).
    *   [x] Migración de `ahci` y `xhci` al nuevo bus.
    *   [x] Migración de `usbhid`, `usb-storage` y `usb-hub` al bus USB.
    *   [ ] `platform_bus` y `virtio_bus` (stubs).
*   [ ] **2.2 Modelo async universal (aur_req)**: **Descartado**. Se ha preferido
    el modelo de primitivas directas (`/dev/dma`, mmap, IRQs) por simplicidad y rendimiento.
*   [ ] **2.3 Colas por dispositivo**: Descartado (sustituido por el modelo directo).

---

## FASE 3 — Módulos Kernel (.kur)
*   [x] **3.1 Formato .kur**: Implementado (`kur.h`, `tools/kurbuild`). Incluye SHA-256.
*   [x] **3.2 ksymtab y reubicaciones**: Implementado (`ksymtab.c`, `kur_loader.c`).
    Soporta `R_X86_64_PC32`, `PLT32`, `32S`, `64`, imports y exports entre módulos.
*   [x] **3.3 Integración con VFS**: Implementado. `kur_load_all("/lib/modules")` escanea
    el initrd y carga los módulos al arrancar (Fase 3.3 cerrada).
*   [x] **3.4 Recarga en caliente (modprobe/rmmod)**: Implementado con la capa Linux (`kernel/kmod_sys.c`): syscalls `init_module`/`finit_module`/`delete_module` (solo euid 0; `-EBUSY` si otro módulo usa sus exports; parámetros y `rmmod -f` ignorados), `/proc/modules` y `/sys/module/<m>/{initstate,refcnt,taint,holders/}`. Se usan los `modprobe`/`rmmod`/`insmod`/`lsmod` reales de kmod 31 (copiados con `scripts/fetch-bin.sh`). `scripts/mk-kmod-tree.sh` (regla `kmod-tree`, parte de `make drivers`) genera `/lib/modules/0.1.0/` con los `.kur` como `kernel/<m>.ko` e índices de `depmod` calculados sobre módulos stub que replican exports/imports. *Validado en QEMU:* `modprobe hello2` carga `hello` primero; `rmmod hello` falla con "in use by: hello2"; `modprobe -r hello2` descarga ambos; `lsmod` y módulo inexistente correctos. *Limitaciones:* añadir un módulo exige reconstruir el árbol (no hay depmod en el guest); sin `modinfo`/`depmod` en el guest; el `.kur` no tiene parámetros ni `modalias`, así que no hay autocarga por hardware; la release está fijada a `0.1.0` (`uname -r`).
    falta la herramienta de userspace para invocarlo y la integración con `bus_add_driver`.

---

## FASE 4 — Drivers de Usuario (.aur)
*   [x] **4.1 Formato .aur**: Implementado (`aur.h`, `tools/aurbuild`). Manifiesto con
    capabilities, IDs de bus, devfds e IPC.
*   [x] **4.2 Carga de .aur y caps**: Implementado (`aur_loader.c`, `process.c`).
    El loader lee `.aur` vía VFS sin cargar todo el binario en memoria.
    `proc->aur.caps` se hereda en `fork` y se consulta vía syscall `aur_info`.
*   [x] **4.3 Primitivas MMIO**: Implementado. `sys_mmap` intercepta `resourceN` de
    sysfs y mapea el BAR físico (Write-Combining) si el proceso tiene `AUR_CAP_MMIO`.
    *Validado leyendo el STATUS del e1000e en userland.*
*   [x] **4.4 Primitivas DMA**: Implementado (`dma_user.c`). `/dev/dma` con
    `ioctl(DMA_ALLOC)` devuelve un buffer de páginas físicamente contiguas.
    *Validado escribiendo un patrón, sincronizando con clflush y leyéndolo de vuelta.*
*   [x] **4.5 Primitivas IRQ**: Implementado (`irq_user.c`). `/dev/irq/<N>` con `epoll`
    y `AUR_CAP_IRQ`. Solo cubre IRQs legacy 0..15 y vectores MSI 0x60..0x6F; un `.aur`
    no puede habilitar MSI por sí mismo ni usar INTx >15 (ahci.aur usa polling por esto).
*   [~] **4.6 Control Channel (VFS node)**: Resuelto por subsistema en lugar de
    `/dev/aur/<driver>/` genérico: `/dev/net0` (`net_proxy.c`) y `/dev/blkproxy`
    (`block_proxy.c`, Fase 5.1). Falta un canal genérico para drivers de otras clases.
*   [x] **4.7 Bus matching user**: Implementado (`aur_bus.c`). Driver fallback del bus PCI
    que escanea `/usr/lib/aur/*.aur`, compara el manifiesto y lanza el `.aur`; habilita
    bus mastering y MEM/IO antes de lanzarlo. Limitación: una instancia por dispositivo
    declinado y el `.aur` no recibe qué BDF le tocó (e1000e.aur lo lleva hardcodeado;
    ahci.aur escanea sysfs y gestiona todos los controladores).

---

## FASE 5 — Migración de Drivers Reales
*   [x] **5.1 Puente de bloque**: Implementado (`block_proxy.c`, ABI en `aur.h`).
    `/dev/blkproxy` (requiere `AUR_CAP_BLOCK_DEVICE`, un único propietario) con 4 ioctls:
    `BLKP_REGISTER` (alta de un disco `sdX`), `BLKP_READY`, `BLKP_FETCH` (petición +
    datos de escritura) y `BLKP_COMPLETE` (resultado + datos de lectura). Todo va por
    ioctl porque `vfs_read/write_for_proc` trocea los chardev a 4 KiB y retiene `f->lock`.
    El submit es asíncrono (`-EINPROGRESS` + `bio_endio`); los bios grandes se trocean en
    el kernel a `max_sectors`. Si el driver muere, los bios pendientes fallan con `-EIO`
    y los discos se dan de baja (la memoria del `block_device_t` no se libera, a propósito).
*   [x] **5.2 ahci.aur**: Implementado, **opt-in** (`make AHCI_USER=1`, que instala el
    marcador `/usr/lib/aur/ahci.user`; con él `ahci.c` del kernel cede el controlador y
    el arranque espera a `BLKP_READY` antes de escanear particiones). Sin el marcador el
    driver de kernel sigue siendo el dueño. `ahci.aur` gestiona todos los HBA AHCI,
    IDENTIFY, READ/WRITE DMA EXT y FLUSH EXT.
    *Validado en QEMU q35:* `sda` aparece vía `ahci.aur`, FAT32 se monta en `/data` y el
    test `blkproxy: round-trip multi-chunk` (1100 sectores, write+flush+read+restore)
    pasa; suite completa 165/165.
    *Limitaciones v1:* un comando a la vez (slot 0, sin NCQ), polling (sin IRQ/MSI), solo
    LBA48 y discos ATA (no ATAPI/CD), sin hot-plug ni reset de HBA, bounce buffer de 256 KiB
    (dos copias por transferencia). Si `ahci.aur` muere, los discos desaparecen y no hay
    reinicio automático del driver (Fase 6.3).
*   [x] **5.3 e1000e.aur**: Implementado (`drivers/aur/e1000e`, `net_proxy.c`). Driver de
    red completo en Ring 3 con MMIO + DMA + IRQ y DHCP/ping validados. Ya no lleva nada
    fijo: localiza el NIC por vendor/device en sysfs, lee la IRQ del config space (cae a
    polling con `epoll` + timeout si no está en 1..15; *ese camino no está probado*) y
    comunica la MAC real al kernel con `NETP_SET_MAC` (validado con una MAC distinta).
*   [x] **5.4 USB split (xhci.aur)**: Implementado, **opt-in** (`make XHCI_USER=1`, marcador
    `/usr/lib/aur/xhci.user`; sin él `xhci.c` del kernel sigue siendo el dueño). El kernel
    conserva `usb_core`, hubs y los drivers de clase; `xhci.aur` implementa lo que hay bajo
    `usb_hcd_t` (anillos, contextos, comandos, eventos, reset de puertos) y `usb_proxy.c`
    traduce cada operación del HCD a peticiones sobre `/dev/usbproxy` (HELLO / FETCH /
    COMPLETE / EVENT, ABI en `aur.h`). El motor del `.aur` es **asíncrono** de un solo hilo
    (un interrupt-IN de HID puede quedar pendiente indefinidamente sin bloquear al resto).
    La política de hotplug (escaneo de puertos, 3 reintentos) vive en el kernel; el `.aur`
    solo ofrece `PORT_STATUS`/`PORT_RESET` y avisa con `EVENT_PORT_CHANGE`.
    *Validado en QEMU q35:* disco `usb-storage` SuperSpeed + teclado + ratón HS, teclado y
    ratón FS detrás de un `usb-hub` (route string, TT), hotplug con `device_add`, y el test
    `xhci.aur: round-trip bulk` (1100 sectores por bulk IN/OUT); suite 166/166. También con
    `e1000e.aur` + `ahci.aur` + `xhci.aur` a la vez.
    *Limitaciones v1:* sin streams/UAS (`bulk_submit`/`bulk_xfer_phys` quedan a NULL, así
    que `usb_uas.c` no se activa), sin isócronos, sin interrupciones (polling del Event Ring
    cada ~0,5 ms), sin MSI, hasta 30 slots, un solo controlador, sin detach (igual que el
    driver de kernel), `scratchpad` implementado pero sin probar (QEMU no lo pide) y nada
    verificado en hardware real. Hay 2 copias por transferencia (kernel↔bounce 64 KiB).
    *Arreglo colateral:* `sysfs` truncaba los BARs de 64 bits (`resourceN` apuntaba a la
    dirección baja); ahora guarda la física completa (`bar_phys`).
*   [ ] **5.5 Input user**: Pendiente.
*   [ ] **5.6 GFX / winsrv.aur**: Pendiente (aunque `winsrv` ya corre como tarea de kernel/init).

---

## FASE 6 — Resiliencia y Hot-plug
*   [x] **6.1 Timeouts por op**: Hecho para los proxies con peticiones bloqueantes. `blkproxy` (`block_proxy.c`): plazo fijo de 30 s por petición. `usbproxy` (`usb_proxy.c`): plazo = `timeout_ms` de la petición + 5 s de gracia (control y bulk/interrupt), 15 s para el resto de operaciones (NEW_DEVICE, CONFIGURE_EP, PORT_*...), y **sin plazo** si `timeout_ms == 0` (el interrupt-IN de un HID espera indefinidamente a propósito). En ambos un watchdog (`blkp-wd` / `uproxy-wd`, vivo mientras el driver está conectado) falla con `-ETIMEDOUT` las vencidas, pendientes o en vuelo; el plazo se renueva al entregar la petición al driver y un COMPLETE tardío recibe `-ENOENT`. `net_proxy` no necesita nada: sus colas no tienen llamantes bloqueados esperando respuesta. Tests: `blkproxy: watchdog expira peticiones`, `usbproxy: plazos por operación`, `usbproxy: watchdog expira peticiones` (los de reap se saltan si el proxy está en uso). *Validado en QEMU* con `XHCI_USER=1` (storage + teclado + ratón, 75 s): round-trip bulk OK y ningún timeout espurio. *Limitaciones:* un watchdog no mata ni reinicia al driver colgado (6.2); si el .aur completa una petición justo cuando vence, el llamante ya puede haber vuelto (ventana de ~1 s, igual que en `close`).
*   [x] **6.2 Watchdog por driver**: Implementado sobre 6.1 y 6.3. Cuando el watchdog de `blkproxy`/`usbproxy` ve vencer una petición, el driver se da por colgado: se le envía SIGKILL (`process_kill()`, nuevo; una vez por conexión) y `aur-sup` lo relanza. *Validado en QEMU* con `AHCI_USER=1`: `kill -STOP` a `ahci.aur` + `dd if=/dev/sda` → `dd` falla con `ETIMEDOUT` a los 30 s, el driver muere (código 137), se relanza (0,5 s) y `sda` se vuelve a registrar. *Limitaciones:* un plazo vencido por un disco lento (>30 s) mata un driver sano; no hay watchdog para `net0` (sin peticiones con plazo) ni para drivers lanzados fuera de `aur_bus` (no se relanzan); tras el reinicio los montajes anteriores de `sda` siguen apuntando al disco viejo. *Arreglo colateral:* la prueba de 6.3 había enviado SIGFPE en vez de SIGKILL (la máscara de señales del kernel indexa por número de señal, no por número-1).
*   [x] **6.3 Driver restart**: Implementado (`aur_bus.c`, hilo `aur-sup`). Los `.aur` lanzados por el bus PCI se supervisan: si terminan con código != 0 (fallo o señal) se recogen con `process_reap_orphan()` (nuevo; antes los huérfanos con ppid 0 quedaban como zombi para siempre) y se relanzan con backoff exponencial 0,5 s·2^n; tras 5 fallos seguidos (el contador se resetea si el driver aguanta 30 s) se abandona. Salida con código 0 = voluntaria, no se reinicia. Máquina de estados cubierta por el test `aur-sup: máquina de estados de reinicio`. *Validado en QEMU:* dos señales fatales (SIGFPE, código 136) a `e1000e.aur` y recuperación completa cada vez (NIC, MAC, DHCP con lease). *Limitaciones:* solo supervisa drivers lanzados por `aur_bus` (no los de `init`); no probado con `xhci.aur` ni con `ahci.aur` caído (los discos se re-registran con el mismo nombre, pero los montajes previos quedan obsoletos); sin watchdog de cuelgue (6.2), solo detecta muerte.
*   [ ] **6.4 Hot-plug PCI**: Pendiente (USB hot-plug ya funciona en xhci).

---

## FASE 7 y 8 — Tooling, Tests y Extras
*   [x] **7.1 aurbuild / kurbuild**: Implementados. `aurbuild --info` imprime el manifiesto.
*   [ ] **7.2-7.4 Matriz de tests**: Pendiente (fuzzing, stress, qemu-matrix).
*   [ ] **8.0 IOMMU/VT-d**: Pendiente. Actualmente el DMA no usa IOMMU, por lo que
    un `.aur` malicioso con `AUR_CAP_DMA` podría corromper RAM física. Mitigado por
    el diseño actual (solo `DMA_ALLOC`, no `DMA_MAP`), pero no es seguro a nivel de hardware.

---

## Notas de Implementación (Desviaciones del Plan Original)

1.  **`aur_req` (io_uring)**: Se ha descartado. El modelo de `mmap` para MMIO y `ioctl`
    para DMA ha demostrado ser suficiente, mucho más rápido de implementar y fácil
    de depurar. Si en el futuro se necesita alto rendimiento (ej: NVMe con millones de IOPS),
    se puede reintroducir un anillo asíncrono para ese driver concreto.
2.  **`kur_load_all`**: En lugar de tener un `/boot/modules.list`, el loader escanea
    directamente `/lib/modules` en el initrd.
3.  **Capabilities**: En lugar de un bitmap complejo con verificación por dispositivo,
    las caps son globales al proceso (ej: `AUR_CAP_MMIO` permite mapear *cualquier* BAR PCI).
    Para un sistema de producción, se debería añadir una lista de BDFs permitidos en el manifiesto.


Diferencia Teórica (.kur vs .aur)
Característica	.kur (Kernel Module)	.aur (User Driver)
Espacio de memoria	Ring 0 (Kernel). Ve y modifica toda la RAM.	Ring 3 (Userland). Aislado en su propio PML4.
Formato ELF	ET_REL (objeto reubicable, como un .ko).	ET_DYN o ET_EXEC (ejecutable, como un .elf).
Cómo se ejecuta	kur_init() se llama desde el loader del kernel.	Se lanza como un proceso normal (execve) con fork.
Acceso al hardware	Directo. Llama a funciones del kernel (ej. pci_read_config, outb).	Indirecto y seguro. Usa mmap (BARs), /dev/dma y /dev/irq.
¿Qué pasa si falla?	Kernel Panic. Un fallo de segmentación tumba el sistema.	SIGSEGV. El kernel mata el proceso y sigue funcionando.
Uso típico	Drivers críticos (AHCI, memoria, buses).	Drivers de usuario, sandboxing, servicios.