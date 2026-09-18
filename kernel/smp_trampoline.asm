; kernel/smp_trampoline.asm
; Trampoline para arrancar APs: real (16) -> protegido (32) -> largo (64).
;
; Se ensambla con `nasm -f bin` y se convierte a .o con objcopy.
;
; El BSP copia el binario a LMPS_TRAMPOLINE_PHYS (0x7000) y escribe en el header:
;   - Offset 0x08 (u64): pml4_phys (OML4 del kernel en memoria física)
;   - Offset 0x10 (u64): entry_virt (Dirección virtual de ap_entry)
;   - Offset 0x18 (u64): stack_top (Top del stack para el AP)
;
; El AP arranca tras la SIPI con CS:IP = 0x0700:0x0000 (físico 0x7000).

BITS 16

smp_trampoline_start:
    jmp short trampoline_code
    nop

    ; --------------------------------------------------------------------------
    ; Header fijo de configuración (a partir de offset 0x08)
    ; --------------------------------------------------------------------------
    align 8
    trampoline_pml4_phys:    dq 0   ; Offset 0x08
    trampoline_entry_virt:   dq 0   ; Offset 0x10
    trampoline_stack_top:    dq 0   ; Offset 0x18

trampoline_code:
    cli
    cld

    mov al, '1'
    out 0xE9, al

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov al, 'a'
    out 0xE9, al

    ; Cargar GDT de 32/64 bits temporal
    o32 lgdt [0x7000 + (gdt_desc - smp_trampoline_start)]

    mov al, 'b'
    out 0xE9, al

    ; Activar modo protegido (CR0.PE = 1)
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    mov al, 'c'
    out 0xE9, al

    ; Far jump a modo protegido de 32 bits (segmento 0x08 = code32)
    jmp dword 0x08:(0x7000 + (pm_entry - smp_trampoline_start))

BITS 32
pm_entry:
    mov al, '2'
    out 0xE9, al

    mov ax, 0x10            ; data32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov al, 'f'
    out 0xE9, al

    ; 1. Habilitar PAE en CR4
    mov eax, cr4
    or  eax, (1 << 5)       ; CR4.PAE
    mov cr4, eax

    mov al, 'g'
    out 0xE9, al

    ; 2. Cargar PML4 físico en CR3 desde el header fijo (0x7000 + 0x08)
    mov eax, [0x7000 + 0x08]
    mov cr3, eax

    mov al, 'h'
    out 0xE9, al

    ; 3. Habilitar Long Mode (LME) y NXE en IA32_EFER MSR (0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8) | (1 << 11)   ; LME (bit 8) | NXE (bit 11)
    wrmsr

    mov al, 'i'
    out 0xE9, al

    ; 4. Activar Paging (CR0.PG = 1)
    mov eax, cr0
    or  eax, 0x80000000     ; CR0.PG
    mov cr0, eax

    mov al, 'j'
    out 0xE9, al

    ; 5. Far jump a Long Mode de 64 bits (segmento 0x18 = code64)
    jmp 0x18:(0x7000 + (lm_entry - smp_trampoline_start))

BITS 64
lm_entry:
    mov al, '3'
    out 0xE9, al

    mov ax, 0x20            ; data64
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov al, 'l'
    out 0xE9, al

    ; CR4: PAE + PGE + OSFXSR + OSXMMEXCPT
    mov rax, cr4
    or  rax, (1 << 7) | (1 << 9) | (1 << 10)
    mov cr4, rax

    mov al, 'm'
    out 0xE9, al

    ; Cargar stack final del AP desde el header (0x7000 + 0x18)
    mov rsp, [0x7000 + 0x18]
    xor rbp, rbp

    mov al, 'n'
    out 0xE9, al

    mov al, '4'
    out 0xE9, al

    ; Cargar dirección virtual de ap_entry desde el header (0x7000 + 0x10)
    mov rax, [0x7000 + 0x10]

    jmp rax

; -------------------------------------------------------------------------
; GDT temporal.
; --------------------------------------------------------------------------
align 16
gdt_base:
    dq 0x0000000000000000    ; 0x00: null descriptor
    dq 0x00CF9A000000FFFF    ; 0x08: code32 (DPL 0, Exec/Read, 32-bit, 4GB)
    dq 0x00CF92000000FFFF    ; 0x10: data32 (DPL 0, Read/Write, 32-bit, 4GB)
    dq 0x00AF9A000000FFFF    ; 0x18: code64 (DPL 0, Exec/Read, 64-bit L=1)
    dq 0x00AF92000000FFFF    ; 0x20: data64 (DPL 0, Read/Write, 64-bit)
gdt_end:

gdt_desc:
    dw gdt_end - gdt_base - 1
    dd 0x7000 + (gdt_base - smp_trampoline_start)

smp_trampoline_end:
