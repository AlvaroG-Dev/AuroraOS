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
    mov [gs:8], rsp              ; cpu_local_data[cpu].user_rsp = RSP usuario
    mov rsp, [gs:0]              ; RSP = kernel_stack
    and rsp, ~0xF                ; alinear a 16

    ; --- Construir el frame de registers_t ---
    ; Empujamos en orden INVERSO al layout del struct, porque cada push
    ; va a una dirección MENOR.

    ; Primero: el "hardware frame" del iretq (ss, rsp, rflags, cs, rip).
    push qword 0x23              ; ss        = USER_DS_RING3
    push qword [gs:8]            ; rsp       = RSP usuario
    push r11                     ; rflags    = RFLAGS usuario
    push qword 0x1B              ; cs        = USER_CS_RING3
    push rcx                     ; rip       = RIP usuario (en RCX al entrar)

    ; Segundo: int_num, error_code (dummies para homogeneidad con ISRs).
    push qword 0                 ; error_code = 0
    push qword -1                ; int_num    = SYSCALL_INT_NUM (0xFFFFFFFFFFFFFFFF)

    ; Tercero: los 15 registros del userland. Los que ya tenemos en
    ; registros los usamos directamente. RBX, RBP, R12-R15 los
    ; empujamos tal cual (SYSCALL no los ha tocado).
    push rax                     ; rax = número de syscall
    push rcx                     ; rcx = ya está en el frame, pero lo empujamos de nuevo
    push rdx                     ; rdx = arg3 original
    push rbx                     ; rbx = valor del userland
    push rbp                     ; rbp = valor del userland
    push rsi                     ; rsi = arg2 original
    push rdi                     ; rdi = arg1 original
    push r8                      ; r8  = arg5 original
    push r9                      ; r9  = valor del userland
    push r10                     ; r10 = arg4 original
    push r11                     ; r11 = ya está en el frame
    push r12                     ; r12 = valor del userland
    push r13                     ; r13 = valor del userland
    push r14                     ; r14 = valor del userland
    push r15                     ; r15 = valor del userland

    ; Ahora RSP apunta al inicio del frame (que es r15 en el struct).
    mov rdi, rsp
    call syscall_handler_c

    ; --- Retorno ---
    ; El handler devuelve uint64_t en RAX. Lo guardamos en el slot rax
    ; del frame (offset 0x70 desde rsp), por si el handler también lo
    ; tocó a través de regs->rax.
    mov [rsp + 0x70], rax

    ; Saltamos rsp al inicio del frame de iretq (offset 0x88 = rip).
    add rsp, 0x88

    ; Limpiar IOPL del RFLAGS que vamos a restaurar (offset 0x10 desde
    ; rsp ahora: rsp+0x00=rip, rsp+0x08=cs, rsp+0x10=rflags).
    and qword [rsp + 0x10], ~0x3000

    ; Forzar IF=1 para que el userland reciba interrupciones.
    or  qword [rsp + 0x10], 0x200

    ; Volver al userland.
    o64 iretq