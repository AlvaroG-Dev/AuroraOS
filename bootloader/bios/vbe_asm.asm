; bootloader/bios/vbe_asm.asm
BITS 32

section .text
global rm_vbe_set_mode

extern gdt_desc

; void rm_vbe_set_mode(uint16_t mode);
rm_vbe_set_mode:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    push edi
    pushfd
    cli

    mov [saved_esp], esp

    ; Guardar el modo que nos pasa C (en el primer argumento de la pila)
    mov ax, [ebp + 8]
    mov [target_mode], ax

    ; 1. Transición PM32 -> PM16
    jmp 0x08:pm16_entry

BITS 16
pm16_entry:
    mov ax, 0x30
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00

    ; 2. Desactivar Modo Protegido (CR0.PE = 0)
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    ; 3. Salto FAR a Modo Real
    jmp 0x0000:rm_entry

rm_entry:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00

    lidt [rm_idt_desc]

    ; 4. Activar el modo VBE (usando el guardado)
    mov ax, 0x4F02
    mov bx, [target_mode]
    or  bx, 0x4000          ; Forzar Linear Framebuffer (bit 14)
    int 0x10

    ; 5. Recargar GDT y volver a PM
    lgdt [gdt_desc]

    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    jmp 0x08:pm16_ret

pm16_ret:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp dword 0x10:pm32_ret

BITS 32
pm32_ret:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, [saved_esp]

    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

section .data
align 4
saved_esp:   dd 0
target_mode: dw 0

align 4
rm_idt_desc:
    dw 0x03FF
    dd 0x00000000

section .note.GNU-stack noalloc noexec nowrite progbits