; kernel/syscall_entry.asm
;
; Entrada desde Ring 3 vía SYSCALL.
; %gs: base apunta a cpu_local_data[cpu] de la CPU actual.
;
; Construimos un frame que coincide EXACTAMENTE con registers_t:
;
;   offset  contenido
;   0x00    r15
;   0x08    r14
;   0x10    r13
;   0x18    r12
;   0x20    r11
;   0x28    r10
;   0x30    r9
;   0x38    r8
;   0x40    rdi
;   0x48    rsi
;   0x50    rbp
;   0x58    rbx
;   0x60    rdx
;   0x68    rcx
;   0x70    rax
;   0x78    int_num       (SYSCALL_INT_NUM)
;   0x80    error_code    (0)
;   0x88    rip           (userland, desde RCX)
;   0x90    cs            (USER_CS_RING3)
;   0x98    rflags        (userland, desde R11)
;   0xA0    rsp           (userland)
;   0xA8    ss            (USER_DS_RING3)
;
; Total: 22 pushes = 176 bytes (múltiplo de 16).
;
; IMPORTANTE:
; SYSCALL es una instrucción especial y no restaura automáticamente los
; registros de propósito general al volver. El ABI SysV permite que una
; función preserve RBX/RBP/R12-R15 entre llamadas, y el compilador de
; userland puede mantener variables vivas en ellos a través de una syscall.
; Por tanto, debemos restaurar TODOS los GPR originales desde el frame antes
; de iretq, dejando únicamente RAX con el valor de retorno de la syscall.

[BITS 64]

extern syscall_handler_c

global syscall_entry

; Constantes de selectores (deben coincidir con gdt.h):
;   USER_CS_RING3 = 0x1B
;   USER_DS_RING3 = 0x23
;   SYSCALL_INT_NUM = 0xFFFFFFFFFFFFFFFF

syscall_entry:
    cli

    ; --- Guardar RSP del usuario y cargar kernel stack ---
    mov [gs:8], rsp
    mov rsp, [gs:0]
    and rsp, ~0xF

    ; --- Construir el frame de registers_t ---
    ; Hardware frame para iretq.
    push qword 0x23
    push qword [gs:8]
    push r11
    push qword 0x1B
    push rcx

    ; int_num + error_code.
    push qword 0
    push qword -1

    ; GPR del usuario, en el orden inverso del struct registers_t.
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call syscall_handler_c

    ; RAX = retorno de la syscall.
    mov [rsp + 0x70], rax

    ; ------------------------------------------------------------------
    ; Restaurar el contexto GPR del userland.
    ;
    ; Dejamos RAX para el valor de retorno. Todos los demás registros
    ; vuelven exactamente al estado que tenían antes de SYSCALL.
    ; ------------------------------------------------------------------
    mov r15, [rsp + 0x00]
    mov r14, [rsp + 0x08]
    mov r13, [rsp + 0x10]
    mov r12, [rsp + 0x18]
    mov r11, [rsp + 0x20]
    mov r10, [rsp + 0x28]
    mov r9,  [rsp + 0x30]
    mov r8,  [rsp + 0x38]
    mov rdi, [rsp + 0x40]
    mov rsi, [rsp + 0x48]
    mov rbp, [rsp + 0x50]
    mov rbx, [rsp + 0x58]
    mov rdx, [rsp + 0x60]
    mov rcx, [rsp + 0x68]
    mov rax, [rsp + 0x70]

    ; Saltar al hardware frame de iretq.
    add rsp, 0x88

    ; Sanitizar RFLAGS antes de iretq. Solo conservamos flags que
    ; userland puede controlar legítimamente; IOPL/NT/VIP/VIF y bits
    ; reservados no pueden llegar al frame de retorno.
    and qword [rsp + 0x10], 0x240FD7
    or  qword [rsp + 0x10], 0x202

    iretq
