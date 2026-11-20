; Guest F2: real -> protegido -> modo largo (CR0/CR4/EFER/CR3 emulados),
; luego MMIO decodificado (dispositivo de prueba en 0xE0000000).
; nasm -f bin largo.asm -o largo.bin ; carga en 0x1000.
bits 16
org 0x1000
    cli
    xor ax, ax
    mov ds, ax
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm32
bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x8000
    ; tablas: PML4 0x3000 -> PDPT 0x4000 -> PD 0x5000 (0..1G, 2M) y PD 0x6000 (3G..)
    mov edi, 0x3000
    xor eax, eax
    mov ecx, 0x4000/4
    rep stosd
    mov dword [0x3000], 0x4003
    mov dword [0x4000], 0x5003
    mov dword [0x4000+3*8], 0x6003
    mov dword [0x5000], 0x83          ; 0..2M
    mov dword [0x6000], 0xE0000083    ; 0xC0000000+0 -> aqui indice 0 = 3G; ver abajo
    mov eax, cr4
    or eax, 0x20                       ; PAE
    mov cr4, eax
    mov eax, 0x3000
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100                      ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000000                 ; PG
    mov cr0, eax
    jmp 0x18:lm64
bits 64
lm64:
    mov dx, 0x3F8
    lea rsi, [rel msg1]
.p1: lodsb
    test al, al
    jz .m
    out dx, al
    jmp .p1
.m:
    ; PD de 3G: entrada 0 mapea 3G..3G+2M; queremos 0xE0000000 = 3.5G = indice 256 -> fuera.
    ; En su lugar se usa la direccion virtual 0xC0000000 (3G) mapeada a GPA 0xE0000000.
    mov ebx, 0xC0000000
    mov eax, [rbx]
    cmp eax, 0xA0A0A0A0
    jne fail
    mov dword [rbx+0x10], 0x12345678
    mov eax, [rbx+0x10]
    cmp eax, 0x12345678
    jne fail
    mov byte [rbx+0x20], 0x7F
    movzx eax, byte [rbx+0x20]
    cmp eax, 0x7F
    jne fail
    lea rsi, [rel msg2]
.p2: lodsb
    test al, al
    jz done
    out dx, al
    jmp .p2
done:
    hlt
fail:
    mov al, 'X'
    out dx, al
    hlt
msg1: db "Modo largo OK", 10, 0
msg2: db "MMIO OK", 10, 0
align 8
gdt: dq 0
     dq 0x00CF9A000000FFFF   ; 0x08 code32
     dq 0x00CF92000000FFFF   ; 0x10 data32
     dq 0x00209A0000000000   ; 0x18 code64
gdtr: dw gdtr - gdt - 1
      dd gdt
