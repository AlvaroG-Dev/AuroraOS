# Plan_Power: apagado, reinicio y gestión de energía (ACPI + APM + LinuxKPI)

Estado: **P0–P2 implementadas** en la rama `claude/power-acpi` (probado en QEMU q35+OVMF con `scripts/power-test.sh off|reboot`; falta probar en la Portégé y el i7). El resto sigue siendo plan. Rama del plan: `claude/power-acpi-apm-plan` (parte de `claude/beautiful-newton-1neyw4`).

Hecho en P0–P2: `kernel/power.c/.h` (notificadores, SIGTERM/SIGKILL, `bdev_flush`, `drivers_shutdown_all`, backends 0xCF9/8042/triple fault/puertos de emulador, `reboot=pci|kbd|triple`), FADT ampliado (SMI_CMD, ACPI_ENABLE, X_PM1x_CNT, flags), S5 con ACPI_ENABLE y SLP_TYP en dos pasos (sin inventar SLP_TYP si falta `_S5`), RESET_REG con soporte PCI y comprobación de `RESET_REG_SUP`, syscall `reboot` (root, `RESTART2`, magics extra), utilidad `power off|reboot|halt`. Pendiente dentro de P1/P2: `_PTS/_GTS` (el intérprete no pasa argumentos), parada de APs por IPI (el vector 0xFE no tiene manejador) y `.shutdown` de PS/2 y de los drivers de disco.
Hardware de prueba real: Toshiba Portégé R30-A (BIOS/CSM, Haswell) e i7-12700KF (UEFI). Verificación rápida en QEMU.

## 1. Estado actual (revisado en el código)

| Pieza | Dónde | Situación |
|---|---|---|
| FADT (PM1a/b_CNT, PM1 evt, GPE, SCI_INT, flags, RESET_REG/valor, DSDT/X_DSDT) | `kernel/acpi.c:113` | Parseado. Faltan `SMI_CMD`, `ACPI_ENABLE/DISABLE`, `X_PM1a_CNT_BLK` (GAS 64 bits), `FACS`, `x_firmware_ctrl`, `PM_TMR` extendido. |
| `_S5_`/`_S4_`/`_S3_` (SLP_TYP) | `kernel/acpi_pm.c:530` | Extraídos del AML con `acpi_aml.c`. |
| `acpi_pm_shutdown()` | `kernel/acpi_pm.c:621` | Escribe PM1a/b_CNT (`SLP_EN`), pero **no** comprueba `SCI_EN`/no activa ACPI, **no** hace `_PTS`/`_GTS`/`_BFS`, y el fallback usa `outw(0x604,0x2000)` (solo QEMU/Bochs) y un `5<<10` inventado. Sin nada más si falla: `hlt`. |
| `acpi_pm_reboot()` | `kernel/acpi_pm.c:635` | RESET_REG (IO o MMIO) y luego `0x64←0xFE`. **No** soporta espacio PCI (2), **no** comprueba el bit `RESET_REG_SUP` (flags bit 10), **no** hay 0xCF9, ResetSystem UEFI ni triple fault. Comentario de `acpi.h` invertido (GAS: 0=memoria, 1=E/S, 2=PCI; el código sí lo trata bien). |
| `acpi_pm_suspend()` S3 | `kernel/acpi_pm.c:652` | Solo escribe SLP_TYP; sin guardar/restaurar CPU, sin vector de despertar en FACS. No usable; fuera de alcance de la 1ª entrega. |
| Syscall `reboot` (Linux nº 169) | `kernel/syscall.c:6199` | Comprueba magics. RESTART hace solo `outb(0x64,0xFE)`; HALT/POWER_OFF hacen `cli; hlt` **sin apagar**. No hay `RESTART2`, `CAD_ON/OFF`, `SW_SUSPEND`, `KEXEC`, comprobación de permisos (root/CAP_SYS_BOOT). |
| `drivers_shutdown_all()` | `kernel/driver.c:201` | Existe (orden inverso) pero **nadie la llama**. `ps2.c` tiene `.shutdown = NULL`. |
| Sincronización de FS/bloque | `bdev_flush()` en `kernel/block.h:148` | Existe; no se invoca al apagar. Sin `sync()` global ni umount de solo-lectura. |
| Botón de encendido / SCI | — | Sin manejador SCI, sin PM1 status/enable, sin evento `_PWRB`/botón fijo. |
| UEFI `ResetSystem` | — | `bootloader/efi` conoce `RuntimeServices` pero el kernel no recibe ni mapea el puntero. |
| APM (INT 15h/53h) | — | No existe. |
| LinuxKPI | `include/linuxkpi/linux/kernel.h:85` (`SYSTEM_*`), `docs/linuxkpi_api.tsv` (`orderly_poweroff`, `emergency_restart` clasificados K5, aún sin implementar), `linux/acpi.h` (stubs) | `kernel/linuxkpi/` no tiene `reboot.c`; `register_reboot_notifier`, `pm_power_off`, `kernel_restart/power_off/halt`, `machine_*`, `register_restart_handler`, `kmsg_dump` no existen o son stubs. Plan_LinuxKPI.md deja ACPI «en stub» (K6/L7: `acpi_evaluate_*`, `_DSM`, opregion). |
| Panic | `kernel/panic.c:212,295` | Termina en `cli; hlt`; no hay reinicio automático configurable (`panic=N`). |

Conclusión: la ruta feliz de ACPI S5 está a medio hacer y todo lo demás (fallbacks, orden de apagado, usuario, LinuxKPI, APM) falta.

## 2. Objetivos y no objetivos

Objetivos: (1) `poweroff`, `reboot`, `halt` fiables en QEMU, Portégé (BIOS) e i7-12700KF (UEFI); (2) apagado ordenado (procesos → FS → drivers → CPUs → hardware); (3) cadena de fallbacks con registro claro en el log; (4) botón de encendido ACPI → apagado limpio; (5) API LinuxKPI coherente para drivers portados (i915 y siguientes) y syscall `reboot` completa; (6) APM para BIOS antiguas sin ACPI.

No objetivos (por ahora): S3/S4 (suspensión/hibernación), kexec real (solo stubs), ACPICA completo, gestión de energía de CPU/dispositivos más allá de lo ya hecho en `acpi_pm_set_max_performance`.

## 3. Arquitectura propuesta

Un único módulo **`kernel/power.c/.h`** (capa de política) por encima de backends (mecanismo):

```
syscall reboot / init / botón ACPI / panic / drivers LinuxKPI
                 │
        power_request(PWR_OFF | PWR_RESTART | PWR_HALT, reason)
                 │
   kernel_shutdown_prepare():  notifiers → sync → procesos → drivers → IRQ/CPUs
                 │
   backends en orden de prioridad (restart y poweroff por separado):
     ACPI(S5/reset reg) → UEFI RT → APM → puertos (0xCF9 / 8042 / QEMU-isa-debug-exit) → triple fault → hlt
```

- Tabla de backends registrable: `struct power_backend { name, prio, probe(), poweroff(), restart() }`. Cada uno devuelve solo si falla; tras cada intento se espera un tiempo (p. ej. 100 ms con TSC/PIT) y se pasa al siguiente. Así LinuxKPI (`register_restart_handler`, `pm_power_off`) se enchufa sin tocar el core.
- Política de reinicio configurable (`reboot=` en cmdline, como Linux): `acpi`, `efi`, `pci` (0xCF9), `kbd`, `triple`, `bios`; por defecto: UEFI→ACPI→0xCF9→kbd→triple en firmware UEFI, y ACPI→0xCF9→kbd→triple en BIOS.
- Un solo hilo/CPU hace el apagado final; el resto de CPUs se detiene por IPI (reutilizar `smp_boot` / IPI de parada de `panic.c`). Interrupciones desactivadas antes del paso final.

## 4. Fases

### P0: Preparativos y arreglos pequeños (sin riesgo)
- Corregir comentario de `reset_reg_space` en `acpi.h`; leer `X_PM1a_CNT_BLK`/`X_PM1b_CNT_BLK` (FADT offsets 0x94/0xA0) y `SMI_CMD (0x30)`, `ACPI_ENABLE (0x34)`, `ACPI_DISABLE (0x35)`, `FACS`, flag `RESET_REG_SUP` (bit 10), `HW_REDUCED_ACPI` (bit 20), `LOW_POWER_S0` y `FIX_RTC`.
- Opción de cmdline `reboot=`, `acpi=off`, `apm=`, `poweroff=`; `power.debug=1` para trazar cada intento por el puerto serie y el log persistente (`docs/Log_Persistente.md`).
- Pruebas de arranque del kernel: añadir a `kernel/tests/kernel_tests.c` tests de parseo de FADT y de selección de backend (sin ejecutar el apagado real).

### P1: Núcleo de apagado ordenado (`power.c`)
1. `power_request()` idempotente (un segundo pedido durante el apagado se ignora, o escala si lleva >N s).
2. Cadena de notificadores del kernel (`reboot_notifier_list`, prioridad) con eventos `SYS_RESTART/HALT/POWER_OFF`; es la base de LinuxKPI.
3. Pasos: mensaje a la consola/GUI («Apagando…»), `SIGTERM` a procesos de usuario, espera corta, `SIGKILL`, `sync()` global + `bdev_flush()` en todos los dispositivos, umount/remount RO de FS escribibles, `drivers_shutdown_all()` (ya existe; hay que llamarla), parar GPU/DMA (importante: dejar bus-masters quietos antes de S5 o reset), apagar IOAPIC/LAPIC, parar APs por IPI.
4. Timeout por paso (watchdog con el timer del kernel) para que un driver colgado no impida apagar: tras N segundos se salta al backend.
5. Auditar los `.shutdown` de drivers: PS/2 (`ps2.c` tiene `NULL`: dejar el 8042 en estado conocido para que el reset por teclado funcione), ATA/AHCI/NVMe (flush de caché, `STANDBY IMMEDIATE` en ATA, para evitar cuenta de «unsafe shutdown»), USB/xHCI (parar controlador), virtio, red, GPU (nativo y LinuxKPI).
6. Tests QEMU: arrancar, ejecutar `poweroff` y comprobar que QEMU termina con `-no-reboot` sin `hlt` colgado; `reboot` seguido de segundo arranque (contador en disco/NVRAM de prueba).

### P2: Apagado ACPI (S5)
Secuencia conforme a la especificación, en este orden:
1. Si `SCI_EN` (PM1_CNT bit 0) es 0 y existe `SMI_CMD`: `outb(SMI_CMD, ACPI_ENABLE)` y esperar hasta que `SCI_EN` suba (timeout 3 s). En máquinas `HW_REDUCED_ACPI` no se hace.
2. Evaluar `\_PTS(5)` y `\_GTS(5)` si existen (con el intérprete de `acpi_aml.c`; verificar primero que soporta `OperationRegion` EC/SystemIO/PCI_Config, que es lo que suelen tocar; si no, registrar y seguir, como hace Linux ante errores de `_PTS`). Igualmente `\_SST`.
3. Obtener SLP_TYPa/b de `\_S5` (ya hecho); aceptar paquetes con enteros de distintos tamaños y, si el DSDT no define `_S5`, no inventar `5<<10`: pasar al siguiente backend.
4. Desactivar interrupciones; limpiar `WAK_STS` y estados pendientes de PM1; habilitar solo `PWRBTN_EN`/RTC si se requiere wake; escribir `SLP_TYPa|SLP_EN` a PM1a_CNT y `SLP_TYPb|SLP_EN` a PM1b_CNT (por I/O o por GAS si viene en `X_PM1x_CNT_BLK`); en Intel respetar la nota de que algunos chipsets requieren dos escrituras separadas (primero SLP_TYP sin SLP_EN, luego con SLP_EN: es lo que hace Linux).
5. Esperar `WAK_STS`/sondear unos ms con `pause`; si no se apaga, siguiente backend.
6. Fallbacks de puertos conocidos de emuladores (solo si DMI/CPUID indica hipervisor): QEMU/Bochs `0x604`/`0xB004` (`0x2000`), VirtualBox `0x4004`, Cloud-Hypervisor `0x600`. Hoy el `0x604` va incondicional; moverlo detrás de detección de VM.

### P3: Reinicio (cadena de fallbacks)
Orden (según `reboot=` y tipo de firmware), con espera y detección de fallo entre cada uno:
1. **ACPI RESET_REG** (si `RESET_REG_SUP`): GAS memoria, E/S **y PCI config** (2; escritura con `pci_cfg_write` bus/dev/func del campo de dirección). Escribir `reset_value`.
2. **UEFI `ResetSystem(EfiResetCold)`** (solo UEFI; ver P3b).
3. **Puerto 0xCF9** (Reset Control Register del PCH/ICH): primero `0x02` (SYS_RST suave), pausa ≥ 50 µs, luego `0x06` (RST_CPU+SYS_RST) y `0x0E` (FULL_RST) si hace falta. Muy fiable en el i7-12700KF (PCH Alder Lake) y en Haswell.
4. **Controlador de teclado 8042**: esperar IBF vacío (`0x64` bit 1) y escribir `0xFE` (pulso de reset); repetir 2–3 veces. Útil en la Portégé.
5. **Triple fault**: cargar IDT de límite 0 y provocar `int3`; último recurso (también en VMs que ignoran todo lo demás).
6. **BIOS warm/cold** (solo BIOS, modo real vía trampolín `smp_trampoline`-like): `0x472←0x1234`/`0x0000` y `jmp F000:FFF0`. Opcional.
7. Si nada funciona: mensaje «Es seguro apagar el equipo» y `hlt`.
- Valor de `0x472`/CMOS «shutdown status» no necesario en 64 bits salvo opción BIOS-reboot.

#### P3b: UEFI RuntimeServices
- El bootloader EFI debe llamar a `SetVirtualAddressMap` (o dejar mapa identidad) y pasar a `boot_info` el puntero a `RuntimeServices` y el mapa de memoria `EfiRuntimeServicesCode/Data`.
- El kernel mapea esas regiones (preferible NX donde sea Data) y llama a `ResetSystem(EfiResetCold/Shutdown, EFI_SUCCESS, 0, NULL)` con la convención MS ABI. Hace falta un thunk `ms_abi` + pila alineada a 16 + `cli`. Riesgos: firmware con bugs de VA map; ofrecer `efi=noruntime` y `reboot=acpi` como salida.
- Ventaja: `EfiResetShutdown` es un apagado válido en UEFI aun sin `_S5`, y `EfiResetCold` evita las rarezas del PCH en placas modernas.

### P4: Botón de encendido y eventos (SCI)
- Instalar manejador de `SCI_INT` (IOAPIC/ISOs ya existen en `acpi.c`), leer `PM1_STS`: `PWRBTN_STS` → `power_request(PWR_OFF, "botón")` tras confirmación en la GUI (si no hay GUI, inmediato); `SLPBTN_STS`, `RTC_STS`, `WAK_STS`.
- Botones control-method (`PNP0C0C` power, `PNP0C0E` sleep, `PNP0C0D` lid): evaluar `_STA`, enlazar con GPEs (`_Lxx/_Exx`) y `Notify`. Requiere GPE básicas en `acpi_aml.c`; **Portégé R30-A usa EC** para botón/tapa: necesita driver mínimo de EC (`PNP0C09`, puertos 0x62/0x66) para `_Qxx`. Dividir: primero botón fijo (PM1), después EC.
- Desde Alder Lake la mayoría de placas usan botón fijo; la Portégé, EC.
- Lid/AC/batería: fuera de esta fase, solo dejar el hook `acpi_notifier_chain` para LinuxKPI (`register_acpi_notifier`).

### P5: APM (BIOS antiguas sin ACPI)
Alcance acotado: solo BIOS legacy, **detección en el bootloader BIOS** (modo real) porque APM usa INT 15h:
1. En el stage del bootloader BIOS: `INT 15h AX=5300h` (verificar instalación, versión ≥ 1.1, flags bit «32-bit interface»/«real-mode»), `AX=5303h` (conexión de interfaz protegida de 32 bits: devuelve segmentos CS/DS y offset de entrada) o, más simple, mantener la **interfaz en modo real**: `AX=5301h` (conexión real), `AX=530Eh` (declarar versión 1.2), `AX=5308h` (activar gestión de energía), `AX=530Fh` (enganchar control de dispositivos).
2. Pasar en `boot_info` un bloque `apm_info` (versión, flags, cseg/offset…) o un bit «APM usable».
3. Apagado: `INT 15h AX=5307h BX=0001h CX=0003h` (estado = Off). Reinicio: APM no define; usar la cadena de P3. Hacerlo desde 64 bits exige volver a modo real: reutilizar el trampolín de `smp_trampoline.asm` (ya salta de real a largo) como thunk largo→real→INT 15h→largo, con mapa identidad en 0–1 MiB.
4. Prioridad de uso: solo si no hay ACPI (`RSDP` ausente) o `acpi=off apm=on`. En el Portégé R30-A (2013) hay ACPI, así que APM se probará en QEMU (SeaBIOS soporta APM con `-machine acpi=off`) y/o Bochs.
5. Idle/CPU-busy (`AX=5305h/5306h`) y eventos de suspensión (`AX=530Bh`): no se implementan.

### P6: Syscall `reboot` y userspace
- `k_reboot` completo: comprobar `uid==0`/capacidad (`CAP_SYS_BOOT` cuando exista), magics `MAGIC2` aceptando las variantes de Linux (`672274793`, `85072278`, `369367448`, `537993216`), comandos `RESTART`, `HALT`, `POWER_OFF`, `RESTART2` (arg cadena: `"bootloader"`, `"recovery"`, `"efi"`, `"acpi"`, `"bios"`, `"warm"`, `"cold"` → selección de backend), `CAD_ON/CAD_OFF` (Ctrl-Alt-Del en TTY envía SIGINT a init, tomando el enlace con `ps2.c`/`tty`), `SW_SUSPEND` → `-EOPNOTSUPP`, `KEXEC` → `-EINVAL` hasta que haya `kexec_load`; `Linux: reboot()` debe sincronizar y avisar al resto antes de llamar a `power_request` (no hace falta que `halt` apague físicamente: `HALT` detiene CPUs y deja el equipo encendido, `POWER_OFF` apaga).
- Syscalls `kexec_load`/`kexec_file_load` como stubs `-ENOSYS`/`-EPERM`, `/proc/sys/kernel/{ctrl-alt-del,poweroff_cmd,panic}` (sysctl, si existe `/proc`).
- Utilidades en `user/`: `poweroff`, `reboot`, `halt`, `shutdown [-h|-r] [now|+min]`, mensaje wall opcional; compatibles con BusyBox (`build_busybox.sh` ya se usa: su `reboot/poweroff/halt` llaman a `reboot(2)` y mandan `SIGTERM/SIGUSR1/SIGUSR2/SIGINT` a PID 1; implementar esa convención en el init de Aurora o hacer que init llame a `power_request`).
- GUI: menú Apple-style «Apagar / Reiniciar / Cerrar sesión» y el botón de encendido (P4) usan el mismo `power_request` mediante syscall (o IPC con el compositor); diálogo de confirmación y cuenta atrás de 60 s.
- Panic: `panic=N` (segundos) en cmdline → `power_request(PWR_RESTART)` desde `panic.c`, usando solo la vía mínima (sin sync/sin locks) con backends marcados «seguros en panic».

### P7: Integración con LinuxKPI
Nuevo `kernel/linuxkpi/kernel/reboot.c` + cabeceras `include/linuxkpi/linux/reboot.h`, `reboot_mode.h` (completar el grupo **K5** de `linuxkpi_api.tsv`):
- Datos: `void (*pm_power_off)(void)`, `enum reboot_mode reboot_mode`, `reboot_default`, `reboot_type`, `system_state` (`SYSTEM_*` ya existe), `poweroff_cmd`.
- Notificadores: `register_reboot_notifier`/`unregister_reboot_notifier` (cadena `blocking_notifier_head` del core P1; `SYS_DOWN`=`SYS_RESTART`, `SYS_HALT`, `SYS_POWER_OFF`), `register_restart_handler`/`unregister_restart_handler`/`do_kernel_restart` (cadena atómica con prioridades `192` por defecto/`128` alta), `register_sys_off_handler`/`unregister_sys_off_handler`/`devm_register_sys_off_handler`/`devm_register_power_off_handler`/`devm_register_restart_handler` (API moderna ≥ 5.17 que usa Linux 6.18: **imprescindible** al fijar la serie 6.18.59), `do_kernel_power_off`.
- Funciones: `kernel_restart(char *cmd)`, `kernel_halt()`, `kernel_power_off()`, `emergency_restart()`, `orderly_poweroff(bool force)`, `orderly_reboot()`, `hw_protection_shutdown(reason, ms_until_forced)` (térmico: i915/amdgpu/hwmon lo llaman), `machine_restart/halt/power_off` (llaman a la tabla de backends del core), `ctrl_alt_del()`, `reboot_pid_ns` (stub), `kmsg_dump` (cabecera ya presente; integrar con log persistente).
- `pm_power_off`: si un driver `.kur` lo asigna, pasa a ser backend `linuxkpi_pm_power_off` con prioridad media en `poweroff`, y respeta `pm_power_off_prepare`.
- Driver model (`device_driver.shutdown`, `bus_type.shutdown`, `device_shutdown()`): hacer que `power.c` recorra la lista de dispositivos LinuxKPI **en orden inverso** y llame `->shutdown` (i915: `intel_display_driver_shutdown`/`i915_driver_shutdown`, que se registra como `pci_driver.shutdown`; amdgpu: `amdgpu_pci_shutdown`) antes de `drivers_shutdown_all()` nativo. Hoy `kernel/linuxkpi/base/` no llama a ningún `shutdown`; comprobar `pci.c` (`pci_driver.shutdown`) y `platform.c`.
- `pm_runtime`/`dev_pm_ops`: `prepare/complete`, `freeze/thaw`, `poweroff/restore` solo se rellenan como llamada «shutdown» (no hace falta suspend).
- ACPI en LinuxKPI (K6): mantener **stubs** (`acpi_evaluate_integer`, `acpi_get_handle`, etc.) y rutarlos al intérprete propio `acpi_aml.c` cuando i915/amdgpu los necesiten (`_DSM`, `_DOD`, opregion), sin portar ACPICA completo. Añadir `acpi_enter_sleep_state(u8)`, `acpi_leave_sleep_state`, `acpi_sleep_prepare` como stubs que devuelvan `AE_NOT_IMPLEMENTED` **excepto** S5 delegando a P2, `acpi_power_off`, `acpi_target_system_state()` (presente en TSV), `acpi_disabled`, `acpi_gbl_reduced_hardware` desde la FADT. Decisión: **no** adoptar ACPICA; las tablas (FADT, FACS, DSDT) y el AML ya tienen implementación propia. Reevaluar en L9+/KPI de energía si hiciera falta `acpi_bus`.
- kexec: stubs (`kexec_in_progress=false`, `machine_kexec_prepare` → `-ENOSYS`, `crash_*` vacíos) para satisfacer `drm`/`mm`/`ttm`; `crash_kexec` no-op.
- Suspend/hibernate LinuxKPI (`pm_suspend`, `freeze_processes`, `suspend_devices_and_enter`, `pm_notifier`, `register_pm_notifier`): stubs que respondan `-EBUSY`/`-EOPNOTSUPP`; `pm_suspend_target_state=PM_SUSPEND_ON`.
- Cargador `.kur`: orden de descarga en el apagado = inverso a la carga; hay que llamar `module_exit` solo si el driver lo requiere (no obligatorio en apagado; preferible `->shutdown`).
- Escaneo: ejecutar `scripts/linuxkpi-scan.py` tras añadir los símbolos para actualizar `docs/linuxkpi_api.tsv`.

### P8: Validación en hardware y QEMU

| Escenario | Esperado |
|---|---|
| QEMU q35 + OVMF, `-no-reboot` | `poweroff` → QEMU sale (ACPI S5), `reboot` → QEMU sale por `-no-reboot` (vía RESET_REG/0xCF9) |
| QEMU q35 + OVMF sin `-no-reboot` | `reboot` reinicia y arranca de nuevo; contar 2 arranques |
| QEMU i440fx + SeaBIOS | S5 por PM1a (PIIX4 en 0x600) y reset por 0xCF9/8042 |
| QEMU `-machine acpi=off` + SeaBIOS | APM (`5307h`), fallo de ACPI diagnosticado |
| QEMU con ACPI sin `_S5` (DSDT editado) | cae a ResetSystem UEFI shutdown o puertos de emulador |
| Portégé R30-A (BIOS) | S5 ACPI; reset: RESET_REG si lo anuncia; si no 0xCF9 (PCH Lynx Point) y 8042; verificar que no queda en negro por GPU (i915 `shutdown`) ni disco `unsafe shutdown` |
| i7-12700KF (UEFI) | `EfiResetShutdown` y S5 ACPI; reinicio 0xCF9 FULL_RST y ResetSystem; comprobar que drivers nativos/NVIDIA (Plan_NVIDIA) dejan la GPU parada |

Pruebas automáticas: ampliar `scripts/qemu-boot-test.sh`/`qemu-drive.py` con casos «shutdown» y «reboot» que esperen la salida del proceso QEMU (códigos de salida distintos), y un test de regresión de la syscall `reboot` (magics inválidos → `EINVAL`, no-root → `EPERM`).
Registro por serie imprescindible: cada backend loguea «intento / resultado / tiempo»; para la Portégé (sin serie) guardar en el log persistente.

## 5. Riesgos y decisiones abiertas

1. **`acpi_aml.c` para `_PTS/_GTS/_S5` en hardware real**: puede fallar en OperationRegions de EC; el apagado nunca debe depender de ello (tratar errores como no fatales).
2. **UEFI RuntimeServices** requieren cambios en el bootloader (VA map, passing). Decidir si basta con mapa identidad (más sencillo; válido si `SetVirtualAddressMap` no se invoca).
3. **APM desde 64 bits** necesita thunk a modo real; es la parte más costosa para el beneficio. Propuesta: dejarla al final y hacerla opcional (`CONFIG_APM`).
4. **S3/S4**: fuera del alcance; el mismo core (`power_request(PWR_SUSPEND)`) deja el hueco.
5. **Orden con GPU**: i915 en la Portégé y NVIDIA en el 12700KF deben quedar parados antes del S5; si no, pantalla colgada y reinicios mal. Añadir `shutdown` real a LinuxKPI/i915 (hito 2 de L7) antes de probar en hardware.
6. **Seguridad**: syscall `reboot` solo root; no exponer `kexec` hasta tener validación de imágenes.
7. **¿`halt` físico o solo parar?** Linux `halt` detiene y deja encendido; BusyBox `halt` llama `RB_HALT_SYSTEM`. Se mantiene esa semántica.

## 6. Orden de entrega recomendado

1. P0 + P1 (core, llamada a `drivers_shutdown_all`, sync) y P2 (S5 correcto): primer commit usable en QEMU y Portégé.
2. P3 (cadena de reinicio, 0xCF9, triple fault, GAS PCI) y P6 (syscall + utilidades).
3. P7 (LinuxKPI `reboot.h`, `sys_off_handler`, `device_shutdown`) justo antes del hito 2 de i915.
4. P3b (UEFI ResetSystem) y P4 (botón/SCI).
5. P5 (APM) y EC de la Portégé.
6. P8 en paralelo desde el paso 1.

Estimación orientativa de código: P0–P3 ≈ 1,2 kLOC; P6 ≈ 0,4 kLOC; P7 ≈ 1,0–1,5 kLOC (en su mayoría stubs); P3b/P4 ≈ 0,8 kLOC; P5 ≈ 0,5 kLOC + thunk.
