; bootloader/bios/trampoline.asm
BITS 32

section .text
global bios_jump_to_kernel

; void bios_jump_to_kernel(uint32_t pml4_phys,
;                          uint32_t kinfo_phys,
;                          uint64_t entry);
bios_jump_to_kernel:
    cli

    ; 'T' = entramos en el trampoline
    mov dx, 0x3F8
    mov al, 'T'
    out dx, al

    mov eax, [esp + 8]
    mov [saved_kinfo], eax
    mov eax, [esp + 12]
    mov [saved_entry], eax
    mov eax, [esp + 16]
    mov [saved_entry + 4], eax

    ; 'P' = PAE
    mov dx, 0x3F8
    mov al, 'P'
    out dx, al

    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; 'C' = CR3 cargado
    mov dx, 0x3F8
    mov al, 'C'
    out dx, al

    mov eax, [esp + 4]
    mov cr3, eax

    ; 'L' = LME
    mov dx, 0x3F8
    mov al, 'L'
    out dx, al

    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; 'G' = PG
    mov dx, 0x3F8
    mov al, 'G'
    out dx, al

    mov eax, cr0
    or  eax, (1 << 31)
    mov cr0, eax

    ; 'D' = a punto de saltar a long mode
    mov dx, 0x3F8
    mov al, 'D'
    out dx, al

    lgdt [gdt64_desc]
    jmp 0x08:lm_entry

BITS 64
lm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Preparar args del kernel: rdi = kinfo (físico, < 4 GiB)
    mov edi, [rel saved_kinfo]

    ; Debug: 'Z' = long mode + segmentos OK
    mov dx, 0x3F8
    mov al, 'Z'
    out dx, al

    ; Debug: 'J' = a punto de saltar
    mov al, 'J'
    out dx, al

    ; Cargar entry en RBX (NO en RAX, para no pisarlo con mov al)
    mov rbx, [rel saved_entry]

    ; Salto final al kernel
    jmp rbx

.halt:
    cli
    hlt
    jmp .halt

align 8
gdt64:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt64_desc:
    dw gdt64_desc - gdt64 - 1
    dq gdt64

section .data
align 8
saved_kinfo: dd 0
saved_entry: dq 0

section .note.GNU-stack noalloc noexec nowrite progbits