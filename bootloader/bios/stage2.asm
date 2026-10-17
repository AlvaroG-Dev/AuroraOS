; bootloader/bios/stage2.asm
BITS 16

section .text.entry
global _start
extern stage2_main

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    ; --- Escribir "S2" en la pantalla VGA (offset 4) ---
    mov ax, 0xB800
    mov es, ax
    mov di, 4
    mov ax, 0x0F53     ; 'S'
    mov [es:di], ax
    add di, 2
    mov ax, 0x0F32     ; '2'
    mov [es:di], ax
    xor ax, ax
    mov es, ax

    mov dl, [0x0600]

    ; A20
    in  al, 0x92
    or  al, 2
    and al, 0xFE
    out 0x92, al

    ; E820
    call do_e820

    ; VBE (Busca el mejor modo disponible)
    call do_vbe

    ; GDT + PE
    lgdt [gdt_desc]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x10:pm_entry

; ===========================================================================
; do_e820
; ===========================================================================
do_e820:
    push es
    push ds
    mov ax, 0x2000
    mov es, ax
    xor di, di
    xor ebx, ebx
    xor bp, bp
    mov edx, 0x534D4150
.next:
    mov eax, 0xE820
    mov ecx, 24
    int 0x15
    pushf
    pop  cx
    test cx, 1
    jnz .done
    cmp eax, 0x534D4150
    jne .done
    inc bp
    add di, 24
    test ebx, ebx
    jz  .done
    cmp bp, 128
    jb  .next
.done:
    pop ds
    pop es
    mov [0x1FFE], bp
    ret

; ===========================================================================
; do_vbe (Busca el mejor modo disponible en lugar de forzar 0x118)
; ===========================================================================
do_vbe:
    push es
    push fs
    push si
    push di
    push cx

    ; 4F00: VbeInfoBlock
    mov ax, 0x4F00
    mov di, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne .fail

    ; El puntero a la lista de modos está en 0x4000 + 0x0E (offset) y 0x4010 (segmento)
    mov ax, [0x400E]
    mov si, ax
    mov ax, [0x4010]
    mov fs, ax

    mov word [best_mode], 0xFFFF
    mov dword [best_res], 0

.search_loop:
    mov cx, [fs:si]
    cmp cx, 0xFFFF
    je .search_done
    add si, 2

    push cx
    push si

    ; 4F01: Obtener info del modo
    mov ax, 0x4F01
    mov di, 0x4200
    int 0x10
    cmp ax, 0x004F
    jne .next_mode

    ; Atributos del modo (offset 0x00): bit 0 (soportado), bit 4 (gráfico), bit 7 (LFB)
    mov ax, [0x4200]
    and ax, 0x0091
    cmp ax, 0x0091
    jne .next_mode

    ; Bits por píxel (offset 0x19): debe ser 32 obligatoriamente
    mov al, [0x4200 + 0x19]
    cmp al, 32
    jne .next_mode

    ; Limitar la resolución máxima a 1920x1080 para evitar que QEMU/Wayland crasheen
    movzx eax, word [0x4200 + 0x12] ; XResolution
    cmp eax, 1920
    ja .next_mode                   ; Si es mayor de 1920, ignorar
    movzx ebx, word [0x4200 + 0x14] ; YResolution
    cmp ebx, 1080
    ja .next_mode                   ; Si es mayor de 1080, ignorar

.check_mem_model:
    ; Modelo de color (offset 0x1B): 6 = Direct Color (RGB)
    mov al, [0x4200 + 0x1B]
    cmp al, 6
    jne .next_mode

    ; Calcular resolución total (Width * Height)
    movzx eax, word [0x4200 + 0x12] ; XResolution
    movzx ebx, word [0x4200 + 0x14] ; YResolution
    imul eax, ebx                   ; EAX = Width * Height
    cmp eax, [best_res]
    jbe .next_mode                  ; Si no es mejor, saltar

    ; ¡Es el mejor hasta ahora!
    mov [best_res], eax
    mov [best_mode], cx

    ; Copiar datos a 0x21000 (VBE_INFO_ADDR)
    push es
    mov ax, 0x2000
    mov es, ax

    mov eax, [0x4200 + 0x28]           ; PhysBasePtr
    mov [es:0x1000 + 0], eax
    movzx eax, word [0x4200 + 0x10]    ; BytesPerScanLine
    mov [es:0x1000 + 4], eax
    movzx eax, word [0x4200 + 0x12]    ; XResolution
    mov [es:0x1000 + 8], eax
    movzx eax, word [0x4200 + 0x14]    ; YResolution
    mov [es:0x1000 + 12], eax
    movzx eax, byte [0x4200 + 0x19]    ; BitsPerPixel
    mov [es:0x1000 + 16], eax
    mov ax, cx                         ; Mode number
    mov [es:0x1000 + 20], ax
    mov byte [es:0x1000 + 22], 1       ; ok = 1

    pop es

.next_mode:
    pop si
    pop cx
    jmp .search_loop

.search_done:
    cmp word [best_mode], 0xFFFF
    jne .done
    ; Si no encontramos nada, caemos a fail

.fail:
    push es
    mov ax, 0x2000
    mov es, ax
    mov dword [es:0x1000 + 0], 0
    mov byte  [es:0x1000 + 22], 0
    pop es

.done:
    pop cx
    pop di
    pop si
    pop fs
    pop es
    ret

; ===========================================================================
; 32-bit protected mode
; ===========================================================================
BITS 32
pm_entry:
    cli
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x9F000

    movzx eax, byte [0x0600]
    push  0x21000
    movzx eax, word [0x1FFE]
    push  eax
    push  0x20000
    movzx eax, byte [0x0600]
    push  eax

    call  stage2_main
    add esp, 16
.halt:
    cli
    hlt
    jmp .halt

; ===========================================================================
; Datos
; ===========================================================================
section .data
align 4
best_mode: dw 0xFFFF
best_res:  dd 0

; ===========================================================================
; GDT
; ===========================================================================
global gdt
global gdt_desc

align 8
gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0x00, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0x00, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0x00, 0x00

gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt