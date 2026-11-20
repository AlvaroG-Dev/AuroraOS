# SeaBIOS para la mini-VM

`bios.bin` es SeaBIOS 1.16.3 **sin parches**, compilado con `config` (128 KiB).
Licencia: LGPL-3.0-or-later. Codigo fuente: https://www.seabios.org/downloads/seabios-1.16.3.tar.gz
(`build.sh` lo descarga y reconstruye el binario identico).

Lo usa `aurora-vm --bios` / `--bios-test`: se instala en `/usr/share/aurora/vm/bios.bin`.
Opciones relevantes de `config`: modo QEMU (el VMM imita el puente i440FX), virtio-blk,
IDE (ATA PIO) y virtio-blk, ACPI, sin SMM/USB; `vgabios.bin` es SeaVGABIOS (Bochs VBE / stdvga, 1234:1111, ROM de 64 KiB del dispositivo VGA de aurora-vm, se instala en `/usr/share/aurora/vm/vgabios.bin`); salida de depuracion por el puerto 0x402 (`--log-bios`).
