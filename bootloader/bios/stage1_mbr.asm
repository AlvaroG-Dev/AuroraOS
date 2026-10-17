; bootloader/bios/stage1_mbr.asm
BITS 16
ORG 0x7C00

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 62
%endif

STAGE2_LBA      equ 1
STAGE2_ADDR     equ 0x7E00

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov [0x0600], dl
    mov [drive], dl

    ; Escribir "S1" en VGA
    mov ax, 0xB800
    mov es, ax
    mov di, 0
    mov ax, 0x0F53
    mov [es:di], ax
    add di, 2
    mov ax, 0x0F31
    mov [es:di], ax
    xor ax, ax
    mov es, ax

    ; DAP
    mov byte  [0x0500], 0x10
    mov byte  [0x0501], 0
    mov word  [0x0502], STAGE2_SECTORS
    mov word  [0x0504], STAGE2_ADDR
    mov word  [0x0506], 0
    mov dword [0x0508], STAGE2_LBA
    mov dword [0x050C], 0

    mov si, 0x0500
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc  .disk_err

    mov dl, [drive]
    jmp 0x0000:STAGE2_ADDR

.disk_err:
    mov si, msg_err
.print:
    lodsb
    test al, al
    jz  .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

drive:      db 0
msg_err:    db "AURORA: disk error", 13, 10, 0

times 446-($-$$) db 0

; Tabla de particiones vacía (el Makefile inyecta la real)
times 64 db 0

dw 0xAA55