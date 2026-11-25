; kernel/syscall_entry.asm
;
; Entrada desde Ring 3 vía SYSCALL.
; Al entrar, swapgs: %gs pasa a cpu_local_data[cpu] (la del usuario queda en
; MSR_KERNEL_GS_BASE) y al salir otro swapgs la devuelve.
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

section .rodata
align 4
kernel_mxcsr: dd 0x1F80
section .text

extern syscall_handler_c
extern fpu_use_xsave
extern fpu_xsave_lo
extern fpu_xsave_hi
extern fpu_area_size

global syscall_entry

; Constantes de selectores (deben coincidir con gdt.h):
;   USER_CS_RING3 = 0x33
;   USER_DS_RING3 = 0x2B
;   SYSCALL_INT_NUM = 0xFFFFFFFFFFFFFFFF

syscall_entry:
    ; Desde userland %gs tiene la base del usuario (ARCH_SET_GS); la del
    ; kernel (cpu_local_data) está en MSR_KERNEL_GS_BASE. SFMASK ya quitó IF.
    swapgs
    cli

    ; --- Guardar RSP del usuario y cargar kernel stack ---
    mov [gs:8], rsp
    mov rsp, [gs:0]
    and rsp, ~0xF

    ; --- Construir el frame de registers_t ---
    ; Hardware frame para iretq.
    push qword 0x2B
    push qword [gs:8]
    push r11
    push qword 0x33
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

    ; Estado FPU/SSE/AVX del usuario: el kernel usa XMM (gcc) y SYSCALL debe
    ; conservarlos. Guardar con xsave/fxsave en la pila (ver isr_stubs.asm).
    ; El slot [regs-8] guarda el puntero al area.
    push qword 0
    mov r12, rsp
    mov eax, [rel fpu_area_size]
    sub rsp, rax
    and rsp, -64
    cmp dword [rel fpu_use_xsave], 0
    je .fx_save
    mov qword [rsp + 512], 0
    mov qword [rsp + 520], 0
    mov qword [rsp + 528], 0
    mov qword [rsp + 536], 0
    mov qword [rsp + 544], 0
    mov qword [rsp + 552], 0
    mov qword [rsp + 560], 0
    mov qword [rsp + 568], 0
    mov eax, [rel fpu_xsave_lo]
    mov edx, [rel fpu_xsave_hi]
    xsave64 [rsp]
    jmp .fpu_saved
.fx_save:
    fxsave64 [rsp]
.fpu_saved:
    ldmxcsr [rel kernel_mxcsr]
    mov [r12], rsp
    lea rdi, [r12 + 8]

    ; El frame ya está completo y la pila del proceso es estable. Permitir
    ; interrupciones durante el manejador C: SYSCALL entra con IF=0 por
    ; SFMASK, y mantenerlo así durante toda la syscall bloquea el timer,
    ; el scheduler y las IRQ de dispositivos durante cualquier operación
    ; lenta (p. ej. VFS/mmap).
    sti
    call syscall_handler_c
    cli

    ; RAX = retorno de la syscall.
    mov [r12 + 8 + 0x70], rax

    ; Restaurar FPU/SSE/AVX del usuario (r12 sobrevive a la llamada C).
    cmp dword [rel fpu_use_xsave], 0
    je .fx_rest
    mov eax, [rel fpu_xsave_lo]
    mov edx, [rel fpu_xsave_hi]
    xrstor64 [rsp]
    jmp .fpu_restored
.fx_rest:
    fxrstor64 [rsp]
.fpu_restored:
    lea rsp, [r12 + 8]               ; quitar area FPU y slot

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

    ; Limpiar IOPL del RFLAGS.
    and qword [rsp + 0x10], ~0x3000

    ; Forzar IF=1 para que el userland reciba interrupciones.
    or  qword [rsp + 0x10], 0x200

    swapgs
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
