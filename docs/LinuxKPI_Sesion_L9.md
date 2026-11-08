# LinuxKPI L9 — IOMMU/VT-d, energía y endurecimiento

Rama `claude/project-thread-dn71ik` (desde `main` 7e4355a4, sin fusionar). Pedido por
Alvaro tras cerrar L7 («Continúa con L9»). Estado al final de la sesión en la última
sección.

## 1. Qué se ha hecho

| Pieza | Dónde | Estado |
|---|---|---|
| IOMMU Intel VT-d (DMAR, DRHD, RMRR, tablas de 3/4 niveles, IOVA, fallos, cuarentena) | `kernel/iommu.c`, `kernel/iommu.h` | hecho, probado en QEMU |
| `dma_map_*` / `dma_alloc_coherent` con IOVA real por dispositivo | `kernel/linuxkpi/dma/mapping.c` | hecho, probado en QEMU |
| `pm_runtime_*` real (get/put, autosuspend, forbid/allow, D3hot de PCI) | `kernel/linuxkpi/base/pm.c`, `pci/pci.c`, `linux/pm_runtime.h` | hecho; **apagado por omisión** (`lkpi.runtime_pm=on`) |
| Suspend/resume de dispositivos (fases prepare/suspend/late/noirq) + IOMMU | `base/pm.c`, `/sys/power/state` | hecho; no entra en S3 real |
| `pci_reset_function` por PM (sin FLR) | `pci/pci.c` | hecho |
| TDR: watchdog de GPU genérico | `misc/tdr.c`, `linux/lkpi_tdr.h` | hecho, probado con dispositivo simulado |

## 2. Política y por qué (importante para el equipo real)

Nada de esto está probado en hardware. Alvaro prueba con cuidado en el PC y la Portégé,
así que **todo lo que puede cambiar el comportamiento de un equipo real está apagado por
omisión**:

* `iommu=on` activa VT-d. Sin él Aurora no toca el hardware (`[IOMMU] desactivado`).
* `lkpi.runtime_pm=on` activa el runtime PM real. Sin él `pm_runtime_*` se comporta
  exactamente como antes de L9 (solo cuenta usos; i915 sigue viendo el dispositivo
  siempre activo). i915 define `CONFIG_PM=1`, por lo que activarlo en la Portégé haría que
  i915 pase a D3 tras 10 s de reposo: probar con el cable del ratón a mano.
* `iommu.igfx=off` deja la iGPU Intel en pass-through aunque `iommu=on`.
* `iommu.fault_limit=N` (por omisión 10000, 0 = sin límite): cuarentena del dispositivo.

Cuando Alvaro lo haya probado en la Portégé conviene cambiar el valor por omisión de
`iommu` a `auto` (decisión suya; el plan decía «obligatoria antes de L7»).

## 3. IOMMU/VT-d (L9.1)

* **Lectura de DMAR** (`acpi_find_table("DMAR")`, nuevo en `acpi.c`): unidades DRHD con sus
  ámbitos (endpoint/puente, ruta de dispositivo→bus resuelta por PCI), `INCLUDE_PCI_ALL` y
  RMRR. Solo segmento 0.
* **Todos los dispositivos entran en pass-through** (`T=10`): los drivers propios del
  kernel (AHCI, xHCI, red) siguen igual. Se rellena el bus entero para que un dispositivo
  que aparezca después no quede bloqueado. Una unidad sin pass-through (`ECAP.PT`), sin 3/4
  niveles o con remapeo de interrupciones activado por el firmware **no se toca**.
* **Dominio privado por dispositivo** para lo que usa la API DMA de LinuxKPI: se crea en
  `dma_configure` del bus PCI (antes del `probe`) y se libera en `dma_cleanup` (después de
  devres: `dmam_*` aún lo necesita). Avisa si el driver se va con páginas mapeadas.
  Engancha escribiendo primero AW/DID y luego la parte baja de la entrada (atómico de 64
  bits), así un dispositivo en marcha (iGPU) no ve un hueco.
* **RMRR** del firmware: se mapean 1:1 en el dominio del dispositivo al que apuntan.
* **IOVA**: asignador de arriba hacia abajo con huecos libres ordenados; respeta la máscara
  DMA del dispositivo (no hace falta bounce), deja libres la IOVA 0 y la ventana de MSI
  `0xFEE00000-0xFEF00000`, y alinea a potencia de dos (hasta 2 MiB) como Linux.
* **Permisos**: `DMA_TO_DEVICE`→solo lectura, `DMA_FROM_DEVICE`→solo escritura. Un
  dispositivo que escribe en una página «solo lectura» falla (probado).
* **Invalidación** por registros (sin cola): contextos e IOTLB globales al cambiar entradas,
  IOTLB por dominio al desmapear (y al mapear si `CAP.CM`, como QEMU). La IOVA se reutiliza
  solo después de invalidar.
* **Fallos**: interrupción propia (vector MSI) o sondeo `iommu_fault_poll`; se cuentan por
  dominio. Superado `iommu.fault_limit` el dispositivo pierde el bus master (cuarentena).
* **Visible a los drivers**: `device_iommu_mapped()` e `iommu_get_domain_for_dev()` ahora
  responden con verdad (i915 usa `intel_vtd_active`).
* Suspensión: `iommu_suspend()` apaga la traducción y `iommu_resume()` reprograma raíz,
  invalidaciones y TE (las tablas viven en RAM).

### Límites conocidos
* Sin coalescencia de segmentos de `sg` (una entrada de IOVA por segmento).
* No se liberan las tablas intermedias al desmapear (se liberan con el dominio).
* Sin QI, sin remapeo de interrupciones, sin ATS, sin superpáginas, sin segmentos > 0.
* Dispositivos tras un puente PCIe→PCI usan el RID del puente: cubierto porque se rellena
  el bus entero, pero no se aísla entre ellos.
* Los drivers propios del kernel (no LinuxKPI) siguen sin aislamiento.
* Un dispositivo que ya hacía DMA antes de recibir su dominio y que no esté en una RMRR
  fallará al cambiar (solo iGPU en la práctica, cubierta por su RMRR).

## 4. Energía (L9.2 / L9.3)

* **Runtime PM**: estados ACTIVE/SUSPENDING/SUSPENDED/RESUMING, contador de usos,
  `put` → `runtime_idle` → suspender, autosuspend con temporizador (la actividad lo
  retrasa), `-EBUSY/-EAGAIN` del driver mantienen activo sin marcar error, `forbid/allow`
  (`power/control`), resume del padre primero. Callbacks elegidos como Linux
  (pm_domain > type > class > bus > driver). Atributos `power/control`, `runtime_status`,
  `runtime_usage`, `autosuspend_delay_ms`, `runtime_cycles` en cada dispositivo PCI.
* **PCI**: `lkpi_pci_pm_ops` (como `pci_pm_ops`): el driver guarda lo suyo, el bus guarda la
  config y pone D3hot; al volver D0 + restore. Un dispositivo sin `runtime_suspend` del
  driver nunca se apaga. Solo se toca lo que tiene driver de LinuxKPI enlazado.
* **Sistema**: `lkpi_pm_suspend_devices()` / `lkpi_pm_resume_devices()` recorren los
  dispositivos en orden de registro (hijos primero al dormir). Un fallo deshace lo hecho.
  `echo devices > /sys/power/state` hace un ciclo sin dormir (como `pm_test=devices`),
  `cat /sys/power/pm_stats` da contadores.
* **Pendiente**: la entrada real en S3 (`acpi_pm_suspend`) no tiene reanudación en Aurora
  todavía (Plan_Power.md); la fase `noirq` corre con las IRQ habilitadas.

## 5. Endurecimiento (L9.4)

* **TDR** (`linux/lkpi_tdr.h`): registro de `busy()/progress()/reset()`; si hay trabajo y el
  contador no avanza en `timeout_ms` → reset (por omisión `pci_reset_function`, ahora con
  reset por PM si no hay FLR); tras `max_resets` en `window_ms` el dispositivo se da por
  perdido y se avisa por uevent `WEDGED=1` (`RESET=1` en cada recuperación). i915 y NVIDIA
  mantienen su propio hangcheck; no se han conectado.
* **Cuarentena por fallos de DMA** (arriba).
* **Fuzz de ioctls DRM**: ya existía (`tests/wayland/guest/fuzz.sh`); no se ha repetido.
* **No hecho**: límites de memoria por cliente (necesita contabilidad por proceso en
  `shmem`/GEM; ver «Pendiente»).

## 6. Pruebas / Resultados

Solo QEMU (q35 + `intel-iommu,caching-mode=on`, `iommu=on iommu.fault_limit=40
lkpi.runtime_pm=on`). **Nada probado en hardware real.**

* Con IOMMU: 213 pruebas, 212 correctas, 1 fallo, 18 omitidas. Base q35 sin IOMMU: 213, 212, 1, 19.
  El único fallo (`test_k5.c:43`) ya existía en q35 y no tiene relación con L9.
* Pruebas nuevas: `lkpi_test_iommu` (dominio, IOVA, alineación, DMA, mapeo de solo lectura,
  IOVA obsoleta, varios mapeos, ciclo suspend/resume con DMA después, cuarentena, liberación y
  reenganche) y `lkpi_test_pm` (runtime PM on/off, orden de suspensión del sistema, ruta de
  error, D3hot PCI, reset por PM, TDR). Para D3hot el virtio-rng debe colgar de un
  `pcie-root-port`.
* Ajustes de las pruebas existentes por el entorno IOMMU: `scripts/qemu-drive.py` añade
  `iommu_platform=on` al virtio-rng cuando hay `intel-iommu` (sin eso QEMU ignora la
  IOMMU para virtio y el anillo no se traduce); el driver de juguete negocia
  `VIRTIO_F_ACCESS_PLATFORM`; `test_k6` espera 0 rebotes en `edu` con IOMMU (la IOVA ya queda
  bajo la máscara de 28 bits).
* La escritura sobre una página de solo lectura no cambia la memoria; QEMU registra el
  fallo con retraso, así que la prueba solo lo comprueba de forma blanda (se ve en el log).
