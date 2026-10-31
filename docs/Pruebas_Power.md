# Pruebas de apagado y reinicio (Portégé R30-A e i7-12700KF)

Fecha: 2026-10-29. Rama `claude/power-uefi-linuxkpi`. Qué comprobar en hardware
real; QEMU ya pasó todo lo marcado (QEMU) con `scripts/power-test.sh` y
`scripts/power-button-test.sh`.

## Antes de empezar

1. `git pull` de la rama y `make iso`. Volcar la ISO al USB (modo DD).
2. Para ver el log sin serie (Portégé): añadir a `bootloader/aurora.conf`
   `cmdline=klog.file=/data/aurora.log` (ver `docs/Log_Persistente.md`). Tras
   cada prueba que cuelgue, el fichero llega hasta ~0,5 s antes del cuelgue.
3. Opciones de `cmdline=` útiles (se pueden combinar con espacios):

| Opción | Efecto |
|---|---|
| `reboot=efi\|pci\|kbd\|triple` | backend de reinicio que se prueba primero |
| `poweroff=efi` | UEFI `ResetSystem` antes que ACPI S5 |
| `efi=noruntime` | no usar servicios runtime de UEFI |
| `acpi.pts=0` | no ejecutar `\_PTS`/`\_GTS` |
| `acpi.button=0` | no sondear el botón de encendido |

Cada prueba: anotar **OK / se cuelga / no hace nada**, y si falla, qué línea
`[POWER]` / `[ACPI PM]` fue la última.

## A. Portégé R30-A (BIOS, Haswell)

| # | Prueba | Cómo | Esperado |
|---|---|---|---|
| A1 | Apagado por defecto | escribir `poweroff` en el shell | se apaga la máquina (luz fuera). Log: `Apagado S5 (SLP_TYPa=… )` |
| A2 | Reinicio por defecto | `reboot` | reinicia y vuelve a arrancar. Anotar qué backend lo hizo (`RESET_REG`, `0xCF9`, `teclado`) |
| A3 | Reinicio forzado por cada backend | tres arranques con `reboot=pci`, `reboot=kbd`, `reboot=triple` y `reboot` | cada uno reinicia; si alguno no, anotar cuál |
| A4 | `halt` | `halt` | pantalla/serie parados con la máquina encendida (no se apaga) |
| A5 | Apagado con GPU i915 | A1 con `lkpi.load=linuxkpi,lkpi_drm,lkpi_ttm,lkpi_drm_display,lkpi_i915` | se apaga sin dejar la pantalla colgada; en el log aparece el `shutdown` de i915 (usar `drm.debug=0x1e lkpi.i915_diag=1` si falla) |
| A6 | Reinicio con GPU i915 | A2 con i915 cargado | vuelve a arrancar y dibuja con i915 |
| A7 | `_PTS`/`_GTS` | A1 y mirar el log | líneas `\_PTS(5)` y `\_GTS(5)`; si no apaga, repetir con `acpi.pts=0` y comparar |
| A8 | Botón de encendido | pulsar una vez el botón en el escritorio | si es botón fijo: apagado ordenado. Si el log dice "botón por método de control: sin soporte", es lo esperado (falta el driver de EC) y no se apaga |
| A9 | Apagado limpio de disco | tras A1, arrancar y mirar el log | sin avisos de FAT32 sucio ni `sync bcache flush falló` |
| A10 | ACPI desactivado en la BIOS (si la Portégé lo permite) | opcional | solo informativo: APM aún no está implementado |

## B. i7-12700KF (UEFI)

| # | Prueba | Cómo | Esperado |
|---|---|---|---|
| B1 | Apagado por defecto | `poweroff` | se apaga (S5 de ACPI) |
| B2 | Reinicio por defecto | `reboot` | reinicia y vuelve a arrancar |
| B3 | UEFI ResetSystem apagado | `poweroff=efi` y `poweroff` | se apaga. Línea `[EFI-RT] ResetSystem(2)` en el log |
| B4 | UEFI ResetSystem reinicio | `reboot=efi` y `reboot` | reinicia. Línea `[EFI-RT] ResetSystem(0)`. **Esta es la prueba de mayor riesgo**: si se cuelga, anotarlo y usar `efi=noruntime` |
| B5 | 0xCF9 y teclado | `reboot=pci`, `reboot=kbd` | reinicia con cada uno |
| B6 | Botón de encendido | pulsar una vez | apagado ordenado (en placas de sobremesa suele ser botón fijo). Con `acpi.button=0` no debe reaccionar |
| B7 | Con NVIDIA/driver gráfico | A1/A2 con la GPU cargada | no queda la pantalla colgada ni el equipo a medio apagar |
| B8 | Arranque en frío tras apagar | apagar con B1 y encender | arranca normal (sin `unsafe shutdown` ni pérdidas en `/data`) |
| B9 | Mapa de memoria runtime | solo leer el log del arranque | línea `[EFI-RT] RuntimeServices=… N regiones runtime`; N>0 |

## C. Comunes (cualquier máquina)

| # | Prueba | Esperado |
|---|---|---|
| C1 | `power off`, `power reboot`, `power halt` | igual que `poweroff`, `reboot`, `halt` |
| C2 | Con el sistema gráfico abierto y una app abierta | las apps reciben SIGTERM y el sistema apaga sin errores de FS |
| C3 | Apagar dos veces seguidas desde dos terminales | no hay doble apagado ni cuelgue raro (el segundo se ignora) |
| C4 | `reboot` como usuario no root | `EPERM` |

## D. QEMU (ya pasadas; repetir si cambias algo)

- `scripts/power-test.sh off` y `scripts/power-test.sh reboot` (q35 + OVMF).
- `CMDLINE="poweroff=efi" scripts/power-test.sh off`, `CMDLINE="reboot=efi" scripts/power-test.sh reboot`.
- `scripts/power-button-test.sh`.
- Con `make run-bios` (SeaBIOS) el reinicio cae a 0xCF9. **Ojo**: ese objetivo usa
  `-no-shutdown`, así que QEMU se queda pausado ("paused (shutdown)") en vez
  de cerrarse; es el comportamiento esperado.

## Qué enviarme si algo falla

El log (`/data/aurora.log`) y, si la máquina no llegó a arrancar, una foto de la
última pantalla. Indicar prueba (A3, B4…), opciones de `cmdline=` y modelo/BIOS.

## Todavía no implementado (no probar)

APM, botón/tapa por EC (`_Qxx`), suspensión S3/S4, Ctrl-Alt-Del, `kexec`.
