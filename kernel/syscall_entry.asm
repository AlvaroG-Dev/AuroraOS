; kernel/syscall_entry.asm
;
; Entrada desde Ring 3 vía SYSCALL.
; %gs: base apunta a cpu_local_data[cpu] de la CPU actual.
;
[BITS 64]

extern syscall_handler_c

global syscall_entry

syscall_entry:
    ; ------------------------------------------------------------------
    ; Entrada desde Ring 3 vía SYSCALL.
    ; Al entrar:
    ;   RCX = RIP usuario, R11 = RFLAGS usuario, RSP = stack usuario,
    ;   RAX = num syscall, RDI/RSI/RDX/R10/R8/R9 = args.
    ; ------------------------------------------------------------------

    ; 1. Guardar RSP usuario y cargar kernel stack per-CPU (%gs)
    mov [gs:8], rsp                  ; cpu_local_data[cpu].user_rsp = RSP usuario
    mov rsp, [gs:0]                  ; RSP = cpu_local_data[cpu].kernel_stack

    ; 2. Alinear RSP a 16 bytes.
    and rsp, ~0xF

    ; 3. Guardar contexto del usuario (16 pushes = 128 bytes, múltiplo de 16)
    push qword [gs:8]                ; RSP usuario
    push r11                         ; RFLAGS usuario
    push rcx                         ; RIP usuario

    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9
    push rax                         ; num syscall

    ; 4. Reorganizar argumentos para syscall_handler_c(num, a1, a2, a3, a4, a5):
    ;    RDI=num, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5
    mov r9, r8
    mov r8, r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax

    call syscall_handler_c

    ; 5. Restaurar registros (RAX = valor de retorno).
    add rsp, 8                       ; descartar num syscall salvado

    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    pop rcx                          ; RIP usuario
    pop r11                          ; RFLAGS usuario
    pop rsp                          ; RSP usuario

    ; 6. Retornar al usuario
    o64 sysret
