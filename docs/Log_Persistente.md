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
