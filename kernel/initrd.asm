; kernel/initrd.asm
; Incrusta el paquete Initramfs TAR directamente en la seccion .rodata del kernel

section .rodata
global initrd_start
global initrd_end

initrd_start:
    incbin "initrd.tar"
initrd_end: