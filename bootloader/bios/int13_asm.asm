; bootloader/bios/int13_asm.asm
BITS 32

section .text
global rm_int13_read

extern gdt_desc   ; GDT definida en stage2.asm

rm_int13_read:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    push edi
    pushfd
    cli

    mov [saved_esp], esp

    ; Preparar DAP (Disk Address Packet) en memoria segura
    ; [ebp + 8]  = drive (uint8_t)
    ; [ebp + 12] = lba (uint32_t)
    ; [ebp + 16] = count (uint16_t)
    ; [ebp + 20] = buf (void *, dirección física < 1MB)
    mov eax, [ebp + 20]      ; buf
    mov edx, eax
    and eax, 0x0F
    mov [dap_offset], ax     ; offset = buf & 0x0F
    shr edx, 4
    mov [dap_segment], dx    ; segment = (buf >> 4) & 0xFFFF

    mov ax, [ebp + 16]       ; count
    mov [dap_count], ax

    mov eax, [ebp + 12]      ; lba
    mov dword [dap_lba], eax
    mov dword [dap_lba + 4], 0

    mov byte [dap], 0x10
    mov byte [dap + 1], 0

    mov al, [ebp + 8]
    mov [drive_byte], al

    ; 1. Transición PM32 -> PM16
    ; Salto FAR al selector de código de 16 bits (0x08 en stage2 GDT)
    jmp 0x08:pm16_entry

BITS 16
pm16_entry:
    ; 2. Cargar selectores de datos de 16 bits (0x30 en stage2 GDT, límite 64KB)
    mov ax, 0x30
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00

    ; 3. Desactivar Modo Protegido (CR0.PE = 0)
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    ; 4. Salto FAR a Modo Real puro (16 bits)
    jmp 0x0000:rm_entry

rm_entry:
    ; 5. Inicializar segmentos reales a base 0
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00

    ; 6. Restaurar IDT de Modo Real (base 0, límite 0x3FF)
    lidt [rm_idt_desc]

    ; 7. Ejecutar lectura INT 13h extensión (AH=0x42)
    sti
    mov ah, 0x42
    mov dl, [drive_byte]
    mov si, dap
    int 0x13
    cli

    ; 8. Verificar éxito via Carry Flag (CF)
    jc .fail
    mov byte [status_byte], 0
    jmp .done

.fail:
    mov al, ah
    test al, al
    jnz .save_err
    mov al, 1
.save_err:
    mov [status_byte], al

.done:
    ; 9. Recargar GDT original de stage2
    lgdt [gdt_desc]

    ; 10. Reactivar Modo Protegido (CR0.PE = 1)
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ; 11. Salto FAR a selector 16-bit de PM (0x08)
    jmp 0x08:pm16_ret

pm16_ret:
    ; 12. Cargar segmentos de datos 32-bit (0x18)
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; 13. Salto FAR a selector 32-bit de PM (0x10)
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

    movzx eax, byte [status_byte]
    test eax, eax
    jz .ok
    mov eax, -1
    ret
.ok:
    xor eax, eax
    ret

section .data
align 4
saved_esp:   dd 0
drive_byte:  db 0
status_byte: db 0

align 4
rm_idt_desc:
    dw 0x03FF
    dd 0x00000000

align 4
dap:
    db 0x10
    db 0
dap_count:
    dw 0
dap_offset:
    dw 0
dap_segment:
    dw 0
dap_lba:
    dq 0

section .note.GNU-stack noalloc noexec nowrite progbits
