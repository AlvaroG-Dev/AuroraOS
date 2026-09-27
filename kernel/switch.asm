; kernel/switch.asm
; Context switch + trampoline para Aurora OS SMP

%define TASK_OFF_RSP        0x00
%define TASK_OFF_FPU_STATE  0x18
%define TASK_OFF_CR3        0x20
%define TASK_OFF_ON_CPU     0x78C
%define TASK_OFF_FS_BASE    0x7B8   ; <-- DEBE COINCIDIR con offsetof(task_t, fs_base)

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

    xor eax, eax
    xchg dword [rdi + TASK_OFF_ON_CPU], eax
    mov eax, 1
    xchg dword [rsi + TASK_OFF_ON_CPU], eax

    mov rax, [rsi + TASK_OFF_CR3]
    test rax, rax
    jz .skip_cr3
    mov rcx, cr3
    cmp rax, rcx
    je .skip_cr3
    mov cr3, rax
.skip_cr3:

    ; [musl] Restaurar FS_BASE de la tarea nueva.
    ; Solo escribimos el MSR si el valor no es 0 (procesos kernel lo dejan
    ; a 0 y no queremos pisar el %fs del kernel sin motivo).
    mov rdi, [rsi + TASK_OFF_FS_BASE]
    test rdi, rdi
    jz .skip_fs
    mov rax, rdi
    shr rdi, 32
    mov rdx, rdi
    mov ecx, MSR_FS_BASE
    wrmsr
.skip_fs:

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

    ; [musl] FS_BASE también al saltar por primera vez a la tarea.
    mov rdi, [rdi + TASK_OFF_FS_BASE]
    test rdi, rdi
    jz .skip_fs_jump
    mov rax, rdi
    shr rdi, 32
    mov rdx, rdi
    mov ecx, MSR_FS_BASE
    wrmsr
.skip_fs_jump:

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