# SeaBIOS para la mini-VM

`bios.bin` es SeaBIOS 1.16.3 con los parches de `parches/`, compilado con `config` (128 KiB; con CONFIG_ATA_DMA: el disco duro se lee por DMA de bus maestro).
`parches/0001-atapi-bloques-drq-grandes.patch`: las lecturas de CD/DVD por la BIOS
(INT 13h) piden bloques DRQ de hasta 0xF800 bytes en vez de uno por sector de 2 KiB,
asi cada bloque cuesta unas pocas salidas de la VM (Windows carga boot.wim, ~500 MB,
asi). `vgabios.bin` no cambia.
Licencia: LGPL-3.0-or-later. Codigo fuente: https://www.seabios.org/downloads/seabios-1.16.3.tar.gz
(`build.sh` lo descarga, aplica `parches/*.patch` y reconstruye el binario).

Lo usa `aurora-vm --bios` / `--bios-test`: se instala en `/usr/share/aurora/vm/bios.bin`.
Opciones relevantes de `config`: modo QEMU (el VMM imita el puente i440FX), virtio-blk,
IDE (ATA PIO y DMA) y virtio-blk, ACPI, sin SMM/USB; `vgabios.bin` es SeaVGABIOS (Bochs VBE / stdvga, 1234:1111, ROM de 64 KiB del dispositivo VGA de aurora-vm, se instala en `/usr/share/aurora/vm/vgabios.bin`); salida de depuracion por el puerto 0x402 (`--log-bios`).
