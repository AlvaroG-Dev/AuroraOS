; kernel/boot.asm
; Punto de entrada del kernel x86_64.
; Se ejecuta en long mode, con la PML4 del bootloader y una pila de kernel
; del bootloader. Antes de saltar a kmain:
;   - limpia .bss
;   - inicializa IA32_GS_BASE a &cpu_local_data[0]
;   - verifica que el MSR se escribió
;   - configura su propia pila en .bss
;   - llama a kmain

section .text
bits 64
global _start

extern kmain
extern __bss_start
extern __bss_end
extern cpu_local_data

_start:
    ; --- 'A' = llegamos al _start ---
    mov dx, 0x3F8
    mov al, 'A'
    out dx, al
    mov al, 10
    out dx, al

    ; ------------------------------------------------------------------
    ; Limpiar BSS.
    ; ------------------------------------------------------------------
    mov rax, __bss_start
    mov rbx, __bss_end
.bss_clear:
    cmp rax, rbx
    jae .bss_done
    mov byte [rax], 0
    inc rax
    jmp .bss_clear
.bss_done:

    ; --- 'B' = BSS limpio ---
    mov dx, 0x3F8
    mov al, 'B'
    out dx, al
    mov al, 10
    out dx, al

    ; ------------------------------------------------------------------
    ; [FIX] Escribir IA32_GS_BASE = &cpu_local_data[0] y verificar.
    ;
    ; MSR 0xC0000101 = IA32_GS_BASE
    ; MSR 0xC0000102 = IA32_KERNEL_GS_BASE
    ;
    ; wrmsr toma el valor en edx:eax (64 bits) y el número de MSR en ecx.
    ; ------------------------------------------------------------------
    mov ecx, 0xC0000101
    mov rax, cpu_local_data
    mov rdx, rax
    shr rdx, 32
    wrmsr

    mov ecx, 0xC0000102
    xor eax, eax
    xor edx, edx
    wrmsr

    ; --- Verificación: leer de vuelta IA32_GS_BASE y comparar ---
    mov ecx, 0xC0000101
    rdmsr
    ; esperado: edx:eax = 0xffffffff811a6d00
    cmp eax, 0x811a6d00
    jne .gs_bad
    cmp edx, 0xffffffff
    jne .gs_bad
    mov al, 'W'                     ; wrmsr OK
    jmp .gs_emit
.gs_bad:
    mov al, 'X'                     ; wrmsr falló o se revirtió
.gs_emit:
    mov dx, 0x3F8
    out dx, al
    mov al, 10
    out dx, al

    ; --- 'G' = GS base inicializada (marca visual fija) ---
    mov dx, 0x3F8
    mov al, 'G'
    out dx, al
    mov al, 10
    out dx, al

    ; ------------------------------------------------------------------
    ; Stack propio (en .bss) y salto a kmain.
    ; ------------------------------------------------------------------
    mov rsp, stack_top

    ; --- 'C' = stack listo, a punto de llamar a kmain ---
    mov dx, 0x3F8
    mov al, 'C'
    out dx, al
    mov al, 10
    out dx, al

    call kmain

.halt:
    cli
    hlt
    jmp .halt

section .bss
align 16
stack_bottom:
    resb 0x40000
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits