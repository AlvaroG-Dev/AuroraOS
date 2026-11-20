; Guest F3: sector de arranque para SeaBIOS + virtio-blk.
; Lee el sector 2 (CHS 0/0/2) con int 13h, comprueba "AURORA-F3" y lo dice por la
; serie (0x3F8). Termina con OUT 0xF4. No usa int 10h (no hay VGA en F3).
bits 16
org 0x7C00
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [drive], dl
    mov si, m_boot
    call puts
    mov ah, 0x02            ; leer
    mov al, 1
    mov ch, 0
    mov cl, 2               ; sector 2
    mov dh, 0
    mov dl, [drive]
    mov bx, 0x7E00
    int 0x13
    jc .fail
    mov si, 0x7E00
    mov di, magic
    mov cx, 9
    repe cmpsb
    jne .fail
    mov si, m_ok
    call puts
    xor al, al
    jmp .end
.fail:
    mov si, m_fail
    call puts
    mov al, 1
.end:
    out 0xF4, al
    cli
    hlt
puts:
    mov dx, 0x3F8
.l: lodsb
    test al, al
    jz .r
    out dx, al
    jmp .l
.r: ret
drive: db 0
magic: db "AURORA-F3"
m_boot: db "MBR arrancado desde SeaBIOS", 10, 0
m_ok:   db "Disco OK", 10, 0
m_fail: db "Disco FALLO", 10, 0
times 510-($-$$) db 0
dw 0xAA55
