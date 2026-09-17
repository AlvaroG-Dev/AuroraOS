# Aurora OS

Sistema operativo bare metal x86_64 con multitarea preventiva y entorno grafico avanzado.

## Arquitectura

- **Arquitectura**: x86_64 (Long Mode)
- **Boot**: UEFI (edk2/GNU-EFI)
- **Paginacion**: 4-level paging
- **Graficos**: Framebuffer UEFI/GOP
- **Scheduler**: Round-Robin preventivo (Fase 3)
- **Memoria**: Manager físico y heap
- **Drivers**: Teclado PS/2, Timer PIT, PCI
- **Sistema de archivos**: Initramfs (TarFS)
- **Entorno grafico**: Compositor básico con ventanas

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
├── kernel/gfx/     # Gráficos y ventanas
│   ├── gfx.c       # Framebuffer básico
│   ├── compositor.h
│   ├── compositor.c
│   ├── window.h
│   ├── window.c
│   ├── font_manager.h
│   ├── font_manager.c
│   ├── theme.h
│   └── fonts/      # Fuentes bitmap
├── kernel/drivers/ # Controladores
│   ├── ps2.c       # Teclado y ratón PS/2
│   ├── ps2.h
│   ├── pci.c       # Bus PCI
│   ├── pci.h
│   ├── rtc.c       # Reloj en tiempo real CMOS
│   └── rtc.h
├── kernel/fs/      # Sistema de archivos
│   ├── tarfs.c     # TarFS para initramfs
│   └── tarfs.h
├── kernel/mm/      # Gestión de memoria
│   ├── pmm.c       # Physical Memory Manager
│   ├── pmm.h
│   ├── heap.c      # Kernel heap (kmalloc/kfree)
│   ├── heap.h
│   ├── buddy.c     # Buddy allocator (para heap)
│   └── buddy.h
└── kernel/core/    # Núcleo y schedulering
    ├── sched.c     # Scheduler round-robin
    ├── sched.h
    ├── syscall.c   # Llamadas al sistema
    ├── syscall.h
    ├── cpu.h
    └── interrupts.h
```

## Notas sobre el toolchain

- **Bootloader**: Puede compilarse con `mingw-w64` (genera PE/EFI directamente) o con `gcc` + `objcopy` (convierte ELF a PE/EFI). El Makefile detecta automaticamente cual esta disponible.
- **Kernel**: Puede compilarse con `x86_64-elf-gcc` (toolchain cruzado) o con `gcc` nativo usando flags freestanding. El Makefile detecta automaticamente cual esta disponible.

## Fases de desarrollo

| Fase | Descripcion | Estado |
|------|-------------|--------|
| 0 | Bootloader UEFI + Carga ELF64 | ✅ |
| 1 | Kernel base (GDT, IDT, Paging, Serial) | ✅ |
| 2 | Gestion de memoria (Frame allocator, kmalloc) | ✅ |
| 3 | Multitarea preventiva + Syscalls | ✅ |
| 4 | Drivers (Teclado, Timer, PCI, AHCI) | 🔲 (Teclado, Timer, PCI implementados; AHCI pendiente) |
| 5 | Sistema de archivos (VFS, ext2/simpleFS) | 🔲 (TarFS para initramfs implementado; VFS y ext2 pendientes) |
| 6 | Entorno grafico avanzado (Compositor, WM) | 🔲 (Compositor básico y ventanas implementados; gestor de ventanas avanzado pendiente) |
| 7 | Userland + libc + toolchain | 🔲 |

## Características actuales

- Bootloader UEFI que carga un kernel ELF64 desde una partición FAT32
- Kernel con soporte para:
  - GDT y IDT completos
  - Paginación de 4 niveles
  - Manager de memoria física (PMM) con allocator de páginas
  - Heap del kernel (kmalloc/kfree) usando algoritmo buddy
  - Scheduler round-robin preventivo con multitarea
  - Llamadas al sistema (syscalls) básicas
  - Controlador PS/2 para teclado y ratón
  - Timer PIT para interrupciones periódicas
  - Reloj en tiempo real (RTC) CMOS
  - Bus PCI para enumeración de dispositivos
  - Framebuffer UEFI/GOP para gráficos
  - Compositor básico con soporte para ventanas
  - Manager de fuentes bitmap
  - Initramfs mediante TarFS (sistema de archivos temporal)
  - Salida de depuración por puerto serie COM1
  - Demostración de tareas en kernel y modo usuario (anillo 3)

## Próximos pasos

1. Implementar driver AHCI para almacenamiento SATA
2. Desarrollar sistema de archivos virtual (VFS) y soporte para ext2
3. Mejorar el gestor de ventanas con características como movimiento, redimensionado y enfoque
4. Portar una libc básica (como musl o partes de glibc) para desarrollo de aplicaciones
5. Crear un toolchain de desarrollo completo (compilador, enlazador, etc.) para userland
6. Implementar soporte de red básico (controladores Ethernet y pila TCP/IP)