; user/crt0.asm
; Punto de entrada para programas de usuario.
; Llama a env_init(envp), main(argc, argv), sys_exit(rax).
;
; El kernel deja el arg block en el tope del stack:
;   [rsp + 0]                  = argc
;   [rsp + 8]                  = argv[0]
;   ...
;   [rsp + 8 + 8*argc]         = NULL       (terminador de argv)
;   [rsp + 16 + 8*argc]        = envp[0]
;   ...
;   [rsp + 16 + 8*(argc+envc)] = NULL       (terminador de envp)
;   ...auxv...

BITS 64

extern main
extern sys_exit
extern env_init

global _start

_start:
    ; rsp está alineado a 16 (setup_arg_block lo garantiza).
    mov rdi, [rsp]           ; argc
    lea rsi, [rsp + 8]       ; argv = &argv[0]

    ; envp = rsp + 16 + 8*argc
    lea rdx, [rsp + 16]
    lea rdx, [rdx + rdi*8]   ; rdx = envp

    ; Llamar a env_init(envp). Guardamos argc/argv porque env_init
    ; puede clobbear rdi/rsi/rax/rcx/rdx/r8-r11.
    push rdi                 ; argc
    push rsi                 ; argv
    mov rdi, rdx             ; arg1 = envp
    call env_init
    pop rsi                  ; argv
    pop rdi                  ; argc

    call main

    mov rdi, rax             ; exit code
    call sys_exit

.hang:
    hlt
    jmp .hang