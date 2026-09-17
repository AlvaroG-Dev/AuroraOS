; user/crt0.asm
; Punto de entrada para programas de usuario
; Llama a main() y luego sys_exit

BITS 64

extern main
extern sys_exit

global _start
_start:
    ; En sistemas ELF64, no hay argumentos en argc/argv en este entorno bare-metal
    ; Llamamos a main() sin argumentos
    call main

    ; main retorna -> pasar su resultado a sys_exit
    mov rdi, rax
    call sys_exit

    ; Nunca se llega aquí
.hang:
    hlt
    jmp .hang