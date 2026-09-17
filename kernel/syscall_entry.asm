; kernel/syscall_entry.asm
[BITS 64]

extern syscall_handler_c
extern cpu_local

global syscall_entry

syscall_entry:
    ; ------------------------------------------------------------------
    ; Entrada desde Ring 3 vía SYSCALL.
    ; No usamos swapgs ni GS. Accedemos a cpu_local con direccionamiento
    ; RIP-relative (el kernel está mapeado en todas las tareas).
    ;
    ; Al entrar:
    ;   RCX = RIP usuario, R11 = RFLAGS usuario, RSP = stack usuario,
    ;   RAX = num syscall, RDI/RSI/RDX/R10/R8/R9 = args.
    ; ------------------------------------------------------------------

    ; 1. Guardar RSP usuario y cargar kernel stack.
    mov [rel cpu_local + 8], rsp     ; cpu_local.user_rsp = RSP usuario
    mov rsp, [rel cpu_local + 0]     ; RSP = cpu_local.kernel_stack

    ; 2. Alinear RSP a 16 bytes. El ABI System V exige RSP ≡ 0 (mod 16)
    ;    justo antes de un CALL. kmalloc puede no garantizar alineación
    ;    a 16 en los stacks de tarea, así que forzamos aquí.
    and rsp, ~0xF

    ; 3. Guardar contexto del usuario (16 pushes = 128 bytes, múltiplo de 16,
    ;    así que RSP ≡ 0 (mod 16) antes del call → correcto).
    push qword [rel cpu_local + 8]   ; RSP usuario
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

    ; 6. Retornar al usuario (sin swapgs).
    o64 sysret