# LinuxKPI: sesión del 26-oct-2026 (fase L3, K4/K5)

Registro de la fase L3 de la capa de compatibilidad con drivers de Linux: modelo de dispositivos, PCI e interrupciones. El plan es `Plan_LinuxKPI.md` (decisiones con fecha en su §11); las sesiones anteriores, `docs/LinuxKPI_Sesion_L0_L1.md` y `docs/LinuxKPI_Sesion_L2.md`.

Estado: **L3 terminada**. **L4 (DMA, `sg_table`, `dma_buf`, firmware, E/S de usuario) no empezada**: espera confirmación del autor.

Resumen:
1. **Aurora** (§2): `bus_del_driver` que desvincula, `irq_free` que espera al handler en curso, memoria por debajo de un límite (DMA con máscara), atributos de `sysfs` con `show`/`store`, INTx de PCI con la polaridad de la MADT, LAPIC por CPU y exports. Tres pruebas del kernel nuevas.
2. **Capa** (§2): K4 (kobject/kref, sysfs en el `/sys` real, `device`/`bus_type`/`device_driver`/`class`, devres/`devm_*`, bus de plataforma) y K5 (`pci_register_driver` sobre el `pci_bus` de Aurora, configuración, BARs, `iomap`, INTx/MSI/MSI-X con destino por CPU, `request_irq` en la IRQ real, FLR sin probar, DMA coherente provisional).
3. **Hito** (§2, pruebas): drivers PCI de Linux de juguete en `.kur` sobre `edu` y `virtio-rng-pci` de QEMU: probe, MMIO, INTx, MSI, MSI-X y DMA en los dos sentidos; y con 4 CPU, vectores repartidos, cambio de afinidad y estrés de MSI.
4. Rama `claude/beautiful-newton-1neyw4`, **sin PR ni merge a `main`** (a petición del autor).

---

## 1. Decisiones tomadas (antes de escribir código)

Se hicieron las preguntas de diseño en una sola ronda, con una recomendación en cada una; el autor delegó la elección («lo más profesional y acorde con el objetivo y entorno actual»), así que se tomaron las recomendaciones:

| Tema | Decisión |
|---|---|
| Drivers de Linux frente a `.aur`/`.kur` | Un driver «sombra» por `pci_driver` en el `pci_bus` de Aurora: el primero que reclama un dispositivo se lo queda (también en hot-plug). Lo que ya reclama un `.aur` (bochs, virtio-gpu, e1000e) no se ofrece a Linux. Opción de arranque para cederlos: pendiente, antes de L5. |
| Contexto de IRQ | Handler en la IRQ real: `in_hardirq()` cierto, `preempt_count()` con `HARDIRQ_OFFSET`; la parte con hilo en kthreads de la capa. |
| `sysfs` | En el `/sys` real (registro dinámico de Aurora con callbacks). |
| Dispositivos del hito | `edu` (MMIO, INTx, MSI, DMA) y `virtio-rng-pci` (MSI-X, virtqueue); añadidos a `scripts/qemu-drive.py`. |
| `on_each_cpu()` | Aplazado a L7 (lo usa `i915`, no los drivers de QEMU). |
| Cambios en Aurora | Aceptados los propuestos (a–e de la ronda de preguntas) y uno más que salió al probar: `ioapic_route_pci_intx` (§3.3). MSI/MSI-X los programa la capa, no Aurora. |
| FLR | Implementado, **sin probar** (no hay PCIe con FLR en la máquina `pc`). |
| `platform_bus` | Básico ya en L3 (lo necesita `simpledrm`). |

Detalle y consecuencias en `Plan_LinuxKPI.md` §11 («26-oct-2026 — L3»). Lo hecho después, a petición del autor, en §9 y en §11 («27-oct-2026 — Tras L3»).

## 2. Qué se ha hecho

### Medida previa (qué API piden de verdad los drivers de QEMU)
Con Linux 6.18.59 (`scripts/fetch-linux-drivers.sh --full --no-nvidia`):
- **`bochs`**: `pci_driver`, `pcim_enable_device`, `pci_resource_start/len`, `pci_set/get_drvdata`, `devm_request_mem_region`/`_region`, `devm_ioremap(_wc)`, `aperture_remove_conflicting_pci_devices`, `dev_pm_ops`. Sin IRQ.
- **`simpledrm`**: no es PCI; `platform_device`/`platform_driver`, `dev_get_platdata`, `devm_*` (`kmemdup`, `kcalloc`, `add_action_or_reset`) y `device_link`.
- **`virtio-gpu`**: casi no usa PCI directamente. Su K4/K5 real está en el núcleo virtio (`drivers/virtio/virtio_pci_*`, `virtio.c`), que el escaneo de L0.3 no mide: `bus_register` (el bus virtio), `device_initialize/add/unregister`, `driver_register`, `dev_groups`/`DEVICE_ATTR`/`sysfs_emit`, `pci_iomap(_range)`, `pci_alloc_irq_vectors_affinity`, `pci_irq_vector`, `request_irq`/`free_irq`/`synchronize_irq`/`disable_irq`, `irq_set_affinity_and_hint`, `pci_find_capability`/`pci_find_ext_capability`, `pci_request_selected_regions`.

Con eso se priorizó: núcleo genérico de dispositivos → `pci_driver` como lo usa `bochs` → IRQ/MSI-X como los usa `virtio_pci` → plataforma.

### Lado Aurora (`kernel/`)
| Cambio | Dónde | Prueba |
|---|---|---|
| `bus_del_driver()` llama a `bus->remove` en los dispositivos del driver y los deja libres (antes quedaban apuntando a un driver que se podía descargar) | `bus.c` | `bus: bus_del_driver desvincula sus dispositivos` |
| `irq_free()` espera al `ack` que esté corriendo en otra CPU (contador por acción en `irq_threaded_dispatch`) | `irq_threaded.c` | indirecta (K5 SMP) |
| `pmm_alloc_pages_below(count, limit)`: páginas contiguas bajo un límite, alineadas como el buddy | `pmm.c` | `pmm: páginas por debajo de un límite` |
| `smp_cpu_lapic_id(cpu)` | `smp_boot.c` | indirecta |
| `sysfs_dyn`: atributos con `show`/`store`, permisos, sin tope de 512 entradas, quitar espera a las llamadas en curso; `sysfs.c` escribe esos ficheros y mezcla lo dinámico en `/sys/bus`, `/sys/bus/pci` y `/sys/bus/pci/devices/<BDF>` | `sysfs_dyn.c`, `sysfs.c` | `sysfs: atributos dinámicos con show/store` |
| `ioapic_route_pci_intx()`: INTx de PCI con los flags del ISO de la MADT si los hay, si no nivel/activa baja | `apic.c` | indirecta (K5, INTx de `edu`) |
| Exports para la capa y para las pruebas (`_Static_assert` sobre `struct device`, `struct driver`, `pci_device_t`, `sysfs_dyn_attr_ops_t`) | `ksymtab_exports.c` | carga de la capa |
| QEMU de las pruebas: `-device edu -device virtio-rng-pci` | `scripts/qemu-drive.py` | — |

### Cabeceras (`include/linuxkpi/`, propias)
`kobject`, `sysfs`, `device` (+ `device/{bus,driver,class,devres}.h`, `dev_printk.h`), `kdev_t`, `uidgid`, `pm`, `pm_runtime` (siempre encendido), `ioport`, `mod_devicetable`, `platform_device`, `pci`, `pci_ids` (subconjunto), `pci_regs` (`uapi/linux/pci_regs.h`, valores de la especificación PCI/PCIe), `irqreturn`, `interrupt.h` (IRQ además de tasklets), `preempt.h` (`in_hardirq()` real), `dma-mapping`, `dma-direction`; `io.h` con `ioread*/iowrite*` que aceptan puertos (`ioport_map`) e `IOMEM_ERR_PTR`; `printk.h` con `struct va_format`.

### Implementación (`kernel/linuxkpi/`)
| Fichero | Contenido |
|---|---|
| `lib/kobject.c` | kobject/kset, atributos y grupos (`is_visible`), enlaces, `sysfs_emit`, unión con `sysfs_dyn` (cada fichero es un nodo con contexto; los enlaces son absolutos, `/sys/...`). |
| `base/core.c` | `struct device`, jerarquía en `/sys/devices`, `uevent`/`dev`, atributos de dispositivo, dispositivos raíz, `device_create`, clases (`/sys/class`, `/sys/devices/virtual/<clase>`), `dev_printk`, `dev_err_probe`. |
| `base/bus.c` | `bus_type`, `device_driver`, match/probe/remove síncronos, probe diferido, `bind`/`unbind` por sysfs, notificadores, búsquedas sobre copias con referencia. |
| `base/devres.c` | devres con grupos, `devm_*` (memoria, acciones, páginas, `ioremap`, `ioremap_resource`). |
| `base/platform.c` | Bus de plataforma: match por nombre o `id_table`, recursos, `platform_device_register_full`, IDs automáticos. |
| `kernel/resource.c` | `request_region`/`request_mem_region` (+ `devm_`), registro propio de regiones ocupadas. |
| `pci/pci.c` | Driver sombra en Aurora, `pci_dev` (caché por B:S.F), configuración, capabilities (y PCIe), BARs medidos al vincular, enable/`pcim`, bus master, regiones, `pci_iomap`/`pcim_iomap(_region)`, búsquedas (`pci_get_device/class/slot`), save/restore, PM, FLR, bus `pci` de la capa con atributos y uevent. |
| `pci/msi.c` | `pci_alloc_irq_vectors(_affinity)`: MSI-X (tabla programada por la capa), MSI de un vector, INTx (compartida entre dispositivos de la capa), reparto de destinos entre CPU, `pci_irq_vector`, `pci_irq_get_affinity`, restaurar tras un reset. |
| `kernel/irq.c` | Descriptores, entrada en la IRQ de Aurora, `request_threaded_irq`/`free_irq`, hilos, ONESHOT, `disable/enable_irq` con IRQ pendiente, `synchronize_irq`, `irq_set_affinity`, `devm_request_irq`, `in_hardirq()`. |
| `kernel/dma.c` | `dma_alloc_coherent`/`dmam_` provisional y máscaras. |

`linuxkpi.kur`: ≈ 494 KiB (antes ≈ 250), 543 exports (antes 297).

### Pruebas (`tests/linuxkpi/`)
- `lkpi_test_k4` (`TEST_FLAG_BLOCKING`): kobject (atributo leído y escrito por el mismo camino que el VFS, grupo con atributo oculto, enlace, release), kref, bus/driver/device (probe al registrar driver y al añadir dispositivo, `-EBUSY` por nombre repetido, `/sys/bus/.../drivers/<drv>/<dev>`, `driver`, `subsystem`, atributos del driver, `uevent` con `DRIVER=` y variables del bus, probe diferido que se reintenta, notificadores, baja con `remove`+devres+release, `unbind`/`bind` por sysfs, `driver_unregister`), devres (grupos anidados, `devm_kasprintf`, `devm_kfree`, `devm_release_action`, liberación al soltar el device), clases (`device_create` con `dev_t`, `class/<cls>/<dev>`, `dev`, `uevent`), plataforma (probe con recursos, región reservada por devres y liberada al desvincular).
- `lkpi_test_k5` (`TEST_FLAG_BLOCKING`), **hito de L3**:
  - `edu` sin driver: configuración, capabilities, `pcie_flr`/`pci_reset_function` → `-ENOTTY`.
  - Driver `kt_edu` (`pcim_enable_device`, `pcim_iomap_region`, `pci_set_master`, `dma_set_mask_and_coherent(28 bits)`): enlaces en `/sys/bus/pci/drivers/kt_edu` y `/sys/bus/pci/devices/<BDF>/driver`; MMIO (ID, prueba de vida, factorial); región ocupada; **INTx** (línea 11 del IOAPIC, handler en hardirq que baja la línea; hilo ONESHOT dos veces); **MSI** (vector 0x6x, CPU efectiva, `disable_irq` anidado con la MSI pendiente reproducida en `enable_irq`, hilo con primario, `devm_request_irq`); **DMA** RAM→edu y edu→RAM con interrupción, bajo 256 MiB y alineado; `pci_unregister_driver` y nuevo registro.
  - Driver `kt_vrng` (virtio 1.x moderno): MSI-X con 2 vectores (`PCI_IRQ_AFFINITY`), virtqueue en una página coherente, 4 peticiones de 64 bytes aleatorios (DMA del dispositivo + interrupción en hardirq en la CPU efectiva).
- `lkpi_test_k5_smp` (`TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP`):
  - los dos vectores MSI-X caen en CPU distintas (0 y 1) y el de la cola se mueve por todas las CPU con `irq_set_affinity`: el handler corre siempre en la pedida (12 de 12 con 4 CPU);
  - estrés de MSI de `edu`: 2×CPU hilos disparando mientras el principal hace 300 veces `disable_irq` (ventana sin handlers) / `enable_irq` / `irq_set_affinity` / `synchronize_irq`: **0 handlers dentro de una ventana deshabilitada** y la MSI llega a todas las CPU.
- Tres pruebas del kernel de los cambios de Aurora (tabla de arriba).

## 3. Fallos y problemas encontrados

1. **`sysfs_emit()` exige un buffer alineado a página** (como Linux) y el de `sysfs_dyn` mide 4 KiB pero no está alineado: todas las lecturas de atributos daban vacío. La capa solo comprueba que no sea NULL.
2. **`dev_printk` pasaba el nivel `KERN_*` como argumento**: el `printk` de la capa solo lo reconoce al principio del formato («`4kt_edu ...`» en el log). Ahora se construye el formato con el nivel delante.
3. **Polaridad de las INTx**: la MADT de QEMU `pc` declara las líneas 5/9/10/11 nivel y **activas a nivel alto** (`flags=0xd`), mientras que la especificación PCI (y `irq_user.c` de Aurora) dice activa a nivel bajo. Se añadió `ioapic_route_pci_intx()` para respetar el ISO cuando existe. `irq_user.c` (INTx de los `.aur`) sigue forzando nivel bajo: no se ha tocado (§7).
4. Una línea INTx compartida entre dos dispositivos de la capa (edu y virtio-rng pueden caer en la misma) necesitaba un descriptor con contador de usuarios; corregido antes de probar.

## 4. Pruebas

Todo en QEMU 8.2.2 TCG, `-m 6G`, un solo QEMU cada vez, con `edu` y `virtio-rng-pci`. La suite del kernel se esperó con `--done "passed, "`.

| Prueba | Al empezar (sin cambios) | Al terminar |
|---|---|---|
| Suite del kernel (`-smp 2`) | 192/192 (16 omitidas) | **198/198** (16 omitidas; +3 de Aurora, +K4, +K5, +K5 SMP) |
| Suite del kernel (`-smp 4`) | 192/192 | **198/198** en 3 de 3 arranques |
| `drm_test`, `sig_test`, `rseq_test` | ALL PASS | ALL PASS |
| `clipboard-x11.sh` | pasa | pasa (el `bash` termina; no se reprodujo el cuelgue) |
| `primary-icons.sh` | pasa | pasa (1 de 1; no se reprodujo el cuelgue de `wl-copy`) |

Estrés con 4 CPU (una ejecución): 68 738 disparos de MSI desde 8 hilos, 4 320 atendidas (los disparos se acumulan en el registro de estado de edu), en las 4 CPU, 0 handlers dentro de una ventana deshabilitada; la MSI-X de virtio-rng llegó a la CPU pedida en 12 de 12 rondas.

Historia: el primer arranque con la capa nueva ya hizo probe de `edu` y `virtio-rng`, y pasaron INTx, MSI, MSI-X, DMA y el estrés SMP; solo fallaban K4/K5 por §3.1 y §3.2 (9 fallos, todos de lectura de atributos). Con eso corregido, 198/198.

## 5. Qué no se ha podido comprobar

- **FLR**: ni `pcie_flr` ni `pci_reset_function` con reset real (la máquina `pc` de QEMU no tiene dispositivos PCIe con FLR). Tampoco reset de bus ni por PM.
- **MSI de varios mensajes**: no se implementa (vectores contiguos y alineados); los drivers piden MSI-X o un solo vector.
- **Hot-plug** de un dispositivo con driver de Linux: el camino es el de Aurora (`device_unregister` → `remove` de la sombra), pero no se ha probado con `device_add`/`device_del` del monitor de QEMU.
- **Drivers de Linux reales**: ningún driver de `drivers/` se ha compilado todavía contra K4/K5 (`bochs` necesita DRM, L5; el núcleo virtio no está en el alcance de L3).
- **INTx compartida con un driver de Aurora** y la polaridad de `irq_user.c` (§3.3, §7).
- KVM y hardware real.

## 6. Contradicciones encontradas (planes frente al código)

- `Plan_LinuxKPI.md` §4 L3 (hito): «`edu` de QEMU para DMA/IRQ… recibe MSI-X» y §6 «`e1000e` para PCI/IRQ». En QEMU 8.2.2 `edu` solo tiene INTx y MSI de un vector, y `e1000e` (8086:10d3) lo reclama `drivers/aur/e1000e`: para MSI-X se usó `virtio-rng-pci`.
- `docs/linuxkpi_api.md`: los 16 símbolos K4 y 40 K5 de `virtio-gpu` no incluyen el núcleo virtio (`drivers/virtio/`), que es donde están sus llamadas reales a PCI, IRQ y al modelo de dispositivos (§2). El escáner solo mira `drivers/gpu`.
- `Plan_LinuxKPI.md` §1: «PCI, BARs 64 bits, MSI». `pci_device_t.bar` es de 32 bits y `sysfs.c` guarda `bar_size` en 32 bits (un BAR de más de 4 GiB se trunca); la capa mide los BARs ella misma.
- `Plan_LinuxKPI.md` §4 L3: «`sysfs` básico sobre `sysfs.c` actual». `sysfs.c` es estático y `sysfs_dyn` solo guardaba texto fijo; hizo falta añadir atributos con callbacks.
- `kernel/pci.c` `pci_enable_msix()`: un único LAPIC para todas las entradas y `pci_disable_msix()` no desmapea la tabla (la capa no usa esas funciones).
- `kernel/ipi.h`: `IPI_VECTOR_CALL` (0xFD) definido sin handler en la IDT.
- `kernel/irq_user.c:150`: INTx no ISA forzadas a nivel bajo, contra el ISO de la MADT de QEMU (§3.3).
- `kernel/idt.h`: 21 vectores MSI de kernel (0x63–0x67, 0x70–0x7F) para todo el sistema, con una sola IDT; `amdgpu`/NVIDIA pedirán más.
- Espacio de configuración solo por 0xCF8/0xCFC (256 bytes): sin capabilities extendidas (SR-IOV, AER, BAR redimensionable de `amdgpu`).

## 7. Pendiente y decisiones abiertas

1. ~~**Antes de L5**: que `aur_bus` pueda no reclamar ciertos ID~~ → hecho: `aur.skip=` en la línea del kernel (§9).
2. `on_each_cpu()`/IPI de función (L7, `i915`).
3. ECAM para el espacio de configuración extendido (L7/L8).
4. Más vectores MSI de kernel o vectores por CPU (L7/L8).
5. ~~`irq_user.c`: polaridad de las INTx de los `.aur`~~ → corregido, también en el modo sin MSI del AHCI (§9).
6. Atributos binarios de sysfs y uevents hacia el userland (L4/L5); `device_link` (para `simpledrm`, L5).
7. MSI de varios mensajes, reset de bus/PM, FLR probado en hardware.
8. De sesiones anteriores: `current` estable y `signal_pending` (K8, L4), módulos > 64 MiB (L8), decisión NVIDIA.
9. **L4**: espera confirmación.

## 8. Commits de la sesión

| Commit | Contenido |
|---|---|
| `9eb46bc4` | Aurora: `bus_del_driver` desvincula, `irq_free` espera, `pmm_alloc_pages_below`, `smp_cpu_lapic_id` |
| `7acf874d` | Aurora: atributos de sysfs con `show`/`store` en el registro dinámico |
| `a296d2a0` | Aurora: exports para L3, tres pruebas del kernel, `edu` y `virtio-rng-pci` en `qemu-drive.py` |
| `a639f0a8` | Aurora: `ioapic_route_pci_intx` (polaridad de la MADT) y exports de prueba |
| `91646926` | Capa: modelo de dispositivos (K4) |
| `377f4fbe` | Capa: PCI, interrupciones y DMA provisional (K5) |
| `36f88c95` | Pruebas K4, K5 y K5 SMP |
| (este commit) | Plan §10/§11, estado, `Wayland_Estado_y_Decisiones.md` (D5) y este registro |

## 9. Después de L3 (27-oct-2026, a petición del autor: puntos 1, 2 y 5 del informe)

- **Línea de comandos del kernel y `aur.skip`** (punto 1). Los bootloaders UEFI y BIOS ya leían `cmdline=` de `aurora.conf` pero no se la pasaban al kernel: ahora va en `kernel_boot_info.cmdline` (la estructura pasa de 408 a 664 bytes) y `kernel/cmdline.c` la guarda. `aur.skip=VVVV:DDDD[,…]` (`DDDD` puede ser `*`) hace que `aur_bus` no lance el `.aur` de esos dispositivos; quedan para un driver del kernel (en L5, `cmdline=aur.skip=1234:1111,1af4:1050` para `bochs` y `virtio-gpu` de Linux). Documentado en `bootloader/aurora.conf`. Probado de punta a punta por UEFI con `aur.skip=1234:1111`: `bochs.aur` no se lanza y la suite pasa (199/199); el camino BIOS solo está compilado (QEMU arranca con OVMF). Prueba del kernel nueva del parser y de la lista.
- **Polaridad de las INTx** (punto 2). `irq_user.c` (INTx de los `.aur`) y el modo sin MSI de `ahci.c` usan `ioapic_route_pci_intx()`. Experimento: con la polaridad antigua (activa a nivel bajo) forzada, la INTx de `edu` sigue llegando, así que **el IOAPIC de QEMU no aplica la polaridad**; el fallo solo podía verse en hardware real, donde manda la MADT.
- **Medida de `virtio-gpu`** (punto 5, §6). `linuxkpi-scan.py` cuenta el núcleo virtio con `virtio-gpu` y `fetch-linux-drivers.sh` extrae `drivers/virtio`. `virtio-gpu`: 590 → 933 símbolos (K4 16 → 49, K5 40 → 253, de ellos 191 de cabeceras virtio); primer hito en QEMU 694 → 1022; el resto de drivers no cambia (`Plan_LinuxKPI.md` §3.1). Con la medida corregida, la capa daba 114 de los 143 símbolos K4/K5 de los tres drivers fuera de las cabeceras virtio; se añadieron los que no son de pantalla (device tree como Linux sin `CONFIG_OF`, `pm_domain`, `device_link` mínimo, `core_initcall`, `PCI_POSSIBLE_ERROR`): **137 de 143**; faltan `aperture` y `vgaarb` (L5).

### Commits después de L3

| Commit | Contenido |
|---|---|
| `58665440` | Aurora: línea de comandos del kernel y `aur.skip` |
| `712e00fe` | Aurora: INTx de los `.aur` y del AHCI con la polaridad de la MADT |
| `d4689617` | L0.3: `virtio-gpu` con el núcleo virtio (escáner, descarga, `linuxkpi_api.md`/`.tsv`) |
| `acb15968` | Capa: lo que faltaba de K4/K5 para el primer hito (DT, `pm_domain`, `device_link`, …) |
| `40f65201` | Plan §3.1/§10/§11, D5 y esta sección |
| `fb581634` | Aurora: despertar perdido en `wait_queue_wake_task` |
| `2522f377` | Aurora: no marcar `TASK_DEAD` antes de despertar al padre en `process_exit` |
| (este commit) | Esta sección y Plan §11 |

### El cuelgue de `bash`/`wl-copy` (investigado y corregido)

En la regresión de estos cambios `primary-icons.sh` se colgó en 4 de 5 ejecuciones (histórico: ~5 de 45); cada cuelgue costaba el *timeout* entero (25 min), de ahí la lentitud. No lo causaban los puntos 1, 2 y 5: es un fallo antiguo de Aurora que se ve más cuando los `.kur` de prueba corren a la vez que el guion (más expulsiones).

- **Síntoma** (volcado temporal de procesos y trazas, no subidas): siempre un padre (`bash`, `timeout`, `wl-copy`) dormido en `wait4()` en su `child_wq` con el hijo ya zombi, a veces en el primer `mkdir` del guion.
- **Causa**: `process_exit()` marcaba la tarea `TASK_DEAD` **antes** de poner el zombi, mandar `SIGCHLD` y despertar al padre. Si el tick la expulsaba en ese hueco, el selector no la volvía a elegir (y `reap_dead_tasks` podía liberarla): el despertar no llegaba nunca. Las trazas lo muestran: el hijo escribe su última línea antes del despertar, queda `st=4` (DEAD) y zombi, y la del despertar no sale.
- **Arreglo** (`2522f377`): `TASK_DEAD` solo en `process_exit_current()`, justo antes de ceder; `free_reaped_zombie()` espera a `TASK_DEAD` y `on_cpu == 0` (cediendo la CPU) antes de liberar el espacio de direcciones del zombi, porque ahora el padre puede recogerlo mientras la tarea termina de salir.
- **Antes** (`fb581634`) se arregló una carrera real pero mucho menos probable: `wait_queue_wake_task()` leía `waiting_on` sin cerrojo y perdía el aviso si el que espera había evaluado ya su condición y aún no se había encolado (ahora, Dekker con `task->wake_pending`). Sola no bastaba: 3 cuelgues en 19.
- **Problema aparte, no tocado**: `alarm`, `setitimer` y `timer_create` no funcionan, así que `timeout 20 …` nunca vence y cualquier cuelgue de un hijo es para siempre en vez de un fallo a los 20 s.

| `primary-icons.sh` | Ejecuciones | Cuelgues |
|---|---|---|
| Sin arreglos (`fin2`, `prim2`) | 5 | 4 |
| Con `fb581634` | 19 | 3 (+1 que tardó 407 s) |
| Compilación con trazas, sin el arreglo de `process_exit` | 15 | 11 |
| Con `2522f377` | 11 | 0 (todas 114–139 s) |
| Compilación con trazas y `2522f377` | 6 | 0 |

**Regresión tras estos cambios** (con `2522f377`): suite del kernel 199/199 con `-smp 2` y dos veces con `-smp 4` (16 omitidas; 17 en una de `-smp 4` por el `iso9660: listado de /cdrom` intermitente de siempre); `drm_test`, `sig_test` y `rseq_test` ALL PASS; `clipboard-x11.sh` pasa 2 de 2; `primary-icons.sh` pasa 17 de 17 entre las tandas de arriba y la regresión.
