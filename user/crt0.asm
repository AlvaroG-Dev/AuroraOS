; user/crt0.asm
; Punto de entrada para programas de usuario
; Llama a main() y luego sys_exit

BITS 64

extern main
extern sys_exit

global _start
_start:
    ; El kernel deja el arg block en el tope del stack:
    ;   [rsp]   = argc
    ;   [rsp+8] = argv[0]
    ;   ...
    mov rdi, [rsp]        ; argc
    lea rsi, [rsp + 8]    ; argv

    call main

    mov rdi, rax          ; exit code
    call sys_exit

.hang:
    hlt
    jmp .hang