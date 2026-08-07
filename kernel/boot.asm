; kernel/boot.asm
; Punto de entrada en ASM para el kernel x86_64
; Args del bootloader ya estan en RDI, RSI, RDX, RCX, R8, R9 (System V AMD64)

section .text
bits 64
global _start

extern kmain
extern __bss_start
extern __bss_end

_start:
    ; Limpiar BSS (direcciones virtuales del higher half)
    mov rax, __bss_start
    mov rbx, __bss_end
.bss_clear:
    cmp rax, rbx
    jae .bss_done
    mov byte [rax], 0
    inc rax
    jmp .bss_clear
.bss_done:

    ; Stack propio
    mov rsp, stack_top

    ; Alinear stack a 16 bytes (System V AMD64 ABI)
    ; call kmain empuja RIP (8 bytes), asi que rsp debe ser 16-byte aligned
    ; ANTES del call. Como stack_top ya esta alineado a 16 bytes (align 16),
    ; y call empuja 8 bytes, rsp+8 estara alineado a 16 bytes en kmain.
    ; PERO: la ABI dice que rsp debe estar alineado a 16 bytes ANTES de call.
    ; stack_top es divisible por 16. call empuja 8 bytes.
    ; En kmain, rsp = stack_top - 8. rsp+8 = stack_top, divisible por 16. (OK)
    ; No necesitamos hacer nada especial porque stack_top ya esta alineado.

    ; kmain recibe args en RDI, RSI, RDX, RCX, R8, R9
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
