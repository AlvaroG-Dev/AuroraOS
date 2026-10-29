# Log persistente en RAM (sin puerto serie)

Para depurar en hardware real sin serie. El bootloader UEFI reserva 256 KiB
(`EfiReservedMemoryType`, fuera del PMM) en una direccion estable por debajo
de 4 GiB y el kernel espeja ahi todo lo que sale por serie/klog (incluido el
texto de un panic). Tras un reinicio **caliente** (boton reset, panic + reset,
triple fault) el log del arranque anterior sigue en RAM y se recupera de tres
formas:

| Donde | Quien | Requisito |
|---|---|---|
| `\aurora\prev.log` en el ESP | bootloader (UEFI SimpleFileSystem) | ESP escribible (pendrive; no en ISO) |
| `/data/last_kmsg.log` | kernel, al montar `/data` | un disco FAT32 montado en `/data` |
| `/proc/last_kmsg` | kernel | arrancar con shell |

Notas:
- Si se corta la corriente, la RAM se pierde: solo vale reset caliente.
- Algunos firmwares (y el OVMF de depuracion de QEMU, que rellena la RAM con
  0xAF) borran la memoria al arrancar; en ese caso `log anterior: 0 bytes`.
  Se comprueba con la linea `[PLOG] ... log anterior: N bytes` del arranque.
- Formato y constantes: `bootloader/include/persist_log.h`. Dos ranuras de
  128 KiB: 0 = arranque actual, 1 = anterior. `kernel_boot_info` lleva
  `plog_base`/`plog_size` (680 bytes en total).
- Pendiente: reinicio automatico en panic (hoy se queda en `cli; hlt`; hay
  que pulsar reset) y volcado desde el camino BIOS (ahi `plog_base` = 0).

## Log en vivo a un fichero del USB (para depurar i915)

- `make iso` anade al final de la imagen una particion MBR 2 FAT32
  `AURORADATA` (`USB_DATA_MB=64`; `USB_DATA_MB=0` la desactiva). Al volcar la
  ISO al USB (dd / Rufus modo DD) el kernel la monta en `/data`
  (`mount_boot_media` ahora tambien prueba particiones FAT32).
- Con `cmdline=klog.file=/data/aurora.log` en `aurora.conf`, el hilo
  `klog-file` vuelca el log nuevo cada 500 ms con fsync. `/data` se monta
  antes de `lkpi_boot_load()`, asi que cubre la carga de i915. Si el USB tarda
  en enumerarse, el hilo reintenta el montaje hasta 60 s.
- Funciona tambien con arranque BIOS (sin region persistente usa un buffer en
  el heap y copia el ring de klog).
- Si el kernel se cuelga con las IRQs cortadas, el hilo no corre: el fichero
  llega hasta ~500 ms antes y el resto esta en la RAM persistente
  (`prev.log` / `last_kmsg.log` tras el reset).
- Opt-in: sin `klog.file` no se escribe nada.

## /data no se monta en UEFI en hardware real (2026-10-29, causa NO confirmada)

Sintoma: pendrive con la ISO volcada, `klog.file=/data/aurora.log`. BIOS monta
`/data`; UEFI (RTX 4060 / i7-12700KF) no monta el FAT32. QEMU+OVMF funciona,
tambien solo-USB (sin CD), con 2 G y con 12 G de RAM, tras d13a6607.

Descartado en QEMU: tabla MBR de la imagen (part 1 FAT16 `EFIBOOT` tipo 0c,
part 2 FAT32 `AURORADATA`), orden de montaje, RAM > 4 GiB, ausencia de CD.
No reproducible sin el hardware. Cambios defensivos hechos (no probados en el
PC real):
- `usb_storage`: clear-halt / reset recovery BOT y esperas en TEST UNIT READY,
  INQUIRY y READ CAPACITY (un pendrive real hace STALL/NOT READY; QEMU no).
- `klog_file_task`: reintento de `/data` cada 10 s, ademas de al aparecer un
  block device.
- Diagnostico: a los 20 s, 60 s y 120 s sin `/data` se escribe un resumen
  `[DIAG]` (block devices, tamano de sector, resultado de FAT32) y se activa
  solo el overlay de logs (el mismo que F12). Opcional `klog.reboot=60`:
  reinicio en caliente (una sola vez) para que el bootloader guarde
  `\aurora\prev.log`.
- Prueba: `scripts/usb-data-test.sh` (USB solo, y USB enchufado en caliente).
