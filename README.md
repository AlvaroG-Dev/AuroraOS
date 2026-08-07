# Aurora OS

Sistema operativo bare metal x86_64 con multitarea preventiva y entorno grafico avanzado.

## Arquitectura

- **Arquitectura**: x86_64 (Long Mode)
- **Boot**: UEFI (edk2/GNU-EFI)
- **Paginacion**: 4-level paging
- **Graficos**: Framebuffer UEFI/GOP
- **Scheduler**: Round-Robin preventivo (Fase 3)

## Dependencias

### Debian/Ubuntu
```bash
sudo apt update
sudo apt install build-essential nasm qemu-system-x86 ovmf gnu-efi mtools
# Opcional (solo si quieres usar mingw en lugar de gcc+objcopy):
sudo apt install mingw-w64
```

### Fedora
```bash
sudo dnf install gcc nasm qemu edk2-ovmf gnu-efi-devel mtools
```

### Arch Linux
```bash
sudo pacman -S base-devel nasm qemu edk2-ovmf gnu-efi mtools
```

## Compilar y ejecutar

```bash
cd aurora-os
make          # Genera aurora.img (FAT32 con EFI + kernel)
make run      # QEMU con UEFI OVMF + salida serial
```

### Debug con GDB
```bash
make run-debug   # QEMU con -s -S (server GDB en puerto 1234)
# En otra terminal:
gdb kernel/kernel.elf -ex "target remote :1234" -ex "break kmain"
```

## Estructura

```
├── bootloader/     # UEFI app (C + GNU-EFI)
│   └── Makefile    # Auto-detecta mingw o gcc+objcopy
├── kernel/         # Kernel monolitico (C + ASM)
│   ├── boot.asm    # Entry point ASM
│   ├── main.c      # kmain() - recibe kernel_boot_info*
│   ├── gdt.c       # Global Descriptor Table
│   ├── idt.c       # Interrupt Descriptor Table + PIC
│   ├── paging.c    # 4-level paging
│   └── serial.c    # Logging COM1
```

## Notas sobre el toolchain

- **Bootloader**: Puede compilarse con `mingw-w64` (genera PE/EFI directamente) o con `gcc` + `objcopy` (convierte ELF a PE/EFI). El Makefile detecta automaticamente cual esta disponible.
- **Kernel**: Puede compilarse con `x86_64-elf-gcc` (toolchain cruzado) o con `gcc` nativo usando flags freestanding. El Makefile detecta automaticamente cual esta disponible.

## Fases de desarrollo

| Fase | Descripcion | Estado |
|------|-------------|--------|
| 0 | Bootloader UEFI + Carga ELF64 | ✅ |
| 1 | Kernel base (GDT, IDT, Paging, Serial) | ✅ |
| 2 | Gestion de memoria (Frame allocator, kmalloc) | 🔲 |
| 3 | Multitarea preventiva + Syscalls | 🔲 |
| 4 | Drivers (Teclado, Timer, PCI, AHCI) | 🔲 |
| 5 | Sistema de archivos (VFS, ext2/simpleFS) | 🔲 |
| 6 | Entorno grafico avanzado (Compositor, WM) | 🔲 |
| 7 | Userland + libc + toolchain | 🔲 |
