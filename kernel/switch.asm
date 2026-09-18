; kernel/switch.asm
; Context switch + trampoline para Aurora OS SMP
;
; task_t offsets (ver sched.h):
;   0x00  rsp        (uint64_t)
;   0x08  stack      (uint64_t*)
;   0x10  id         (uint32_t)
;   0x14  state      (task_state_t / uint32_t)
;   0x18  fpu_state  (uint64_t)  <- puntero alineado al buffer FPU
;   0x20  cr3        (uint64_t)  <- CR3 fisico del espacio de direcciones
;   0x28  fpu_raw    (uint8_t[528])

section .text
bits 64

global task_switch

; task_switch(task_t *old_task [rdi], task_t *new_task [rsi], spinlock_t *lock [rdx])
task_switch:
    ; --- Guardar contexto de la tarea vieja (old_task = rdi) ---
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; 1. Guardar RSP actual en old_task->rsp (offset 0x00)
    mov [rdi], rsp

    ; 2. Guardar estado FPU/SSE (offset 0x18 es el puntero al buffer alineado)
    mov rax, [rdi + 0x18]
    test rax, rax
    jz .skip_fxsave
    fxsave64 [rax]
.skip_fxsave:

    ; 2b. Liberar sched_lock si se pasó en rdx (después de guardar old completamente)
    test rdx, rdx
    jz .skip_unlock
    mov dword [rdx], 0
.skip_unlock:

    ; 3. Restaurar FPU/SSE del nuevo proceso (offset 0x18)
    mov rax, [rsi + 0x18]
    test rax, rax
    jz .skip_fxrstor
    fxrstor64 [rax]
.skip_fxrstor:

    ; 4. Cargar nuevo RSP desde new_task->rsp (offset 0x00)
    mov rsp, [rsi]

    ; 5. Cambiar espacio de direcciones: cargar CR3 de new_task (offset 0x20)
    mov rax, [rsi + 0x20]
    test rax, rax
    jz .skip_cr3
    mov rcx, cr3
    cmp rax, rcx
    je .skip_cr3
    mov cr3, rax
.skip_cr3:

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret

global task_jump_to
task_jump_to:
    mov rax, [rdi + 0x18]
    test rax, rax
    jz .skip_fxrstor_jump
    fxrstor64 [rax]
.skip_fxrstor_jump:

    mov rsp, [rdi]

    mov rax, [rdi + 0x20]
    test rax, rax
    jz .skip_cr3_jump
    mov rcx, cr3
    cmp rax, rcx
    je .skip_cr3_jump
    mov cr3, rax
.skip_cr3_jump:

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
    mov ax, 0x1B
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
