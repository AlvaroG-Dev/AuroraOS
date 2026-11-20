; Guest F2: PIT + PIC + HLT + inyeccion de IRQ. 10 ticks de IRQ0 -> "Timer OK".
bits 16
org 0x1000
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x8000
    mov word [0x08*4], isr
    mov word [0x08*4+2], 0
    mov al, 0x34            ; canal 0, lo/hi, modo 2
    out 0x43, al
    mov al, 0xA9            ; 1193 = ~1 kHz
    out 0x40, al
    mov al, 0x04
    out 0x40, al
    mov al, 0xFE            ; solo IRQ0
    out 0x21, al
    sti
.w: hlt
    cmp word [count], 10
    jb .w
    mov dx, 0x3F8
    mov si, msg
.p: lodsb
    test al, al
    jz .d
    out dx, al
    jmp .p
.d: cli
    hlt
isr:
    inc word [count]
    push ax
    mov al, 0x20
    out 0x20, al
    pop ax
    iret
count: dw 0
msg: db "Timer OK", 10, 0
