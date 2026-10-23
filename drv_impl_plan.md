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
*   [ ] **3.4 Recarga en caliente (modprobe/rmmod)**: `kur_loader_unload` funciona, pero
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
*   [ ] **4.5 Primitivas IRQ**: Pendiente. Crear `/dev/irq/<N>` con soporte para `epoll`
    y `AUR_CAP_IRQ` para que un `.aur` pueda dormir hasta que su hardware interrumpa.
*   [ ] **4.6 Control Channel (VFS node)**: Pendiente. Exponer `/dev/aur/<driver>/`
    para que el kernel y el `.aur` se comuniquen.
*   [ ] **4.7 Bus matching user**: Pendiente. Que el kernel lance automáticamente el
    `.aur` cuando detecta hardware que coincide con su manifiesto.

---

## FASE 5 — Migración de Drivers Reales
*   [ ] **5.1 Wrappers de Bloque**: Pendiente.
*   [ ] **5.2 ahci.aur**: Pendiente.
*   [ ] **5.3 e1000e.aur**: Pendiente. *Este es el hito más importante: validar que un
    driver de red completo puede correr en userland usando MMIO + DMA (y quizás polling en lugar de IRQ).*
*   [ ] **5.4 USB split (xhci.aur)**: Pendiente.
*   [ ] **5.5 Input user**: Pendiente.
*   [ ] **5.6 GFX / winsrv.aur**: Pendiente (aunque `winsrv` ya corre como tarea de kernel/init).

---

## FASE 6 — Resiliencia y Hot-plug
*   [ ] **6.1 Timeouts por op**: Pendiente.
*   [ ] **6.2 Watchdog por driver**: Pendiente.
*   [ ] **6.3 Driver restart**: Pendiente.
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