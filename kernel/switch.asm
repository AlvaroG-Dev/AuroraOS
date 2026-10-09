; kernel/switch.asm
; Context switch + trampoline para Aurora OS SMP

%define TASK_OFF_RSP        0x00
%define TASK_OFF_FPU_STATE  0x18
%define TASK_OFF_CR3        0x20
%define TASK_OFF_ON_CPU     0x794
%define TASK_OFF_FS_BASE    0x7C0

%define MSR_FS_BASE 0xC0000100

section .text
bits 64

global task_switch

; task_switch(task_t *old_task [rdi], task_t *new_task [rsi], spinlock_t *lock [rdx])
task_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi + TASK_OFF_RSP], rsp

    mov rax, [rdi + TASK_OFF_FPU_STATE]
    test rax, rax
    jz .skip_fxsave
    fxsave64 [rax]
.skip_fxsave:

    ; Publicar el nuevo propietario de la CPU ANTES de soltar sched_lock.
    ;
    ; La versión anterior liberaba sched_lock mientras new_task seguía
    ; con on_cpu=0 y old_task seguía con on_cpu=1. En SMP otra CPU podía
    ; observar una transición incompleta entre current_task/state/on_cpu.
    ; En particular, new_task ya estaba publicado como TASK_RUNNING en
    ; sched.c, pero todavía no estaba publicado como "en CPU".
    ;
    ; Orden correcto:
    ;   1) new->on_cpu = 1 mientras sched_lock sigue cogido.
    ;   2) liberar sched_lock.
    ;   3) cambiar a la pila de new.
    ;   4) old->on_cpu = 0 ya desde la pila de new.
    ;
    ; Así ninguna CPU puede seleccionar new durante la transición y old
    ; permanece protegido hasta que hemos abandonado su stack.
    mov eax, 1
    xchg eax, [rsi + TASK_OFF_ON_CPU]

    test rdx, rdx
    jz .skip_unlock
    mov dword [rdx], 0
.skip_unlock:

    mov rax, [rsi + TASK_OFF_FPU_STATE]
    test rax, rax
    jz .skip_fxrstor
    fxrstor64 [rax]
.skip_fxrstor:

    mov rsp, [rsi + TASK_OFF_RSP]

    ; Ya estamos ejecutando sobre la pila de new_task. A partir de aquí
    ; old_task ya no está siendo usado por esta CPU y puede publicarse
    ; como libre para que otra CPU lo seleccione.
    xor eax, eax
    xchg eax, [rdi + TASK_OFF_ON_CPU]

    mov rax, [rsi + TASK_OFF_CR3]
    test rax, rax
    jz .skip_cr3
    mov rcx, cr3
    cmp rax, rcx
    je .skip_cr3
    mov cr3, rax
.skip_cr3:

    ; [fs_base] Escribir SIEMPRE, aunque el valor sea 0.
    ; Si no lo hacemos, un kernel thread con fs_base=0 hereda el MSR
    ; del thread anterior (TLS de usuario), lo que corrompe %fs en
    ; tareas sin TLS. En SMP esto contamina CPUs.
    mov rdx, [rsi + TASK_OFF_FS_BASE]
    mov rax, rdx
    shr rdx, 32
    mov ecx, MSR_FS_BASE
    wrmsr

    ; Verificación: leer el MSR de vuelta y comparar.
    push rdx
    mov ecx, MSR_FS_BASE
    rdmsr
    shl rdx, 32
    or rax, rdx
    pop rdx
    cmp rax, [rsi + TASK_OFF_FS_BASE]
    je .fs_ok
    ; mismatch: log por serial
    push rax
    mov al, '!'
    out 0x3F8, al
    pop rax
.fs_ok:

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret

global task_jump_to
task_jump_to:
    mov rax, [rdi + TASK_OFF_FPU_STATE]
    test rax, rax
    jz .skip_fxrstor_jump
    fxrstor64 [rax]
.skip_fxrstor_jump:

    mov rsp, [rdi + TASK_OFF_RSP]

    mov rax, [rdi + TASK_OFF_CR3]
    test rax, rax
    jz .skip_cr3_jump
    mov rcx, cr3
    cmp rax, rcx
    je .skip_cr3_jump
    mov cr3, rax
.skip_cr3_jump:

    ; [fs_base] Idem: escribir siempre.
    mov rdx, [rdi + TASK_OFF_FS_BASE]
    mov rax, rdx
    shr rdx, 32
    mov ecx, MSR_FS_BASE
    wrmsr

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret

global task_trampoline
extern task_entry_wrapper

task_trampoline:
    sti
    mov rdi, r12
    call task_entry_wrapper
.hang:
    hlt
    jmp .hang

global user_fork_return

; Continúa la ejecución en userland a partir del frame construido por
; sched_create_forked_user_task().
user_fork_return:
    mov ax, 0x23
    mov ds, ax
    mov es, ax

    pop r11
    pop r10
    pop r9
    pop r8
    pop rax
    pop rcx
    pop rdx
    pop rsi
    pop rdi

    iretq

global user_trampoline

user_trampoline:
    mov ax, 0x23
    mov ds, ax
    mov es, ax

    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq