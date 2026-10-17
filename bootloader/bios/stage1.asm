BITS 16
ORG 0x7C00

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    ; ============ PRIMERO guardar DL ============
    mov [0x0600], dl

    ; --- Init COM1 (destruye DX) ---
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 1
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA
    xor al, al
    out dx, al
    mov dx, 0x3FC
    mov al, 0x03
    out dx, al

    ; --- "S1\n" ---
    mov dx, 0x3F8
    mov al, 'S'
    out dx, al
    mov al, '1'
    out dx, al
    mov al, 10
    out dx, al

    ; Saltar a stage2.
    jmp 0x0000:0x7E00

times 510-($-$$) db 0
dw 0xAA55