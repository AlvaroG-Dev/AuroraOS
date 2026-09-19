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
;   0x78C on_cpu      (volatile int)  <- [FIX] actualizado aquí

%define TASK_OFF_RSP        0x00
%define TASK_OFF_FPU_STATE  0x18
%define TASK_OFF_CR3        0x20
%define TASK_OFF_ON_CPU     0x78C

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

    ; 1. Guardar RSP actual en old_task->rsp
    mov [rdi + TASK_OFF_RSP], rsp

; 2. Guardar estado FPU/SSE
;
; FXSAVE64 requiere que la dirección destino esté alineada a 16 bytes.
; task->fpu_state apunta a task->fpu_raw redondeado hacia arriba a 16.
; Ver sched.h para el _Static_assert que verifica que el redondeo no
; desborda el buffer.
mov rax, [rdi + TASK_OFF_FPU_STATE]
test rax, rax
jz .skip_fxsave
fxsave64 [rax]
.skip_fxsave:

    ; 3. Liberar sched_lock si se pasó en rdx
    test rdx, rdx
    jz .skip_unlock
    mov dword [rdx], 0
.skip_unlock:

    ; 4. Restaurar FPU/SSE del nuevo proceso
    mov rax, [rsi + TASK_OFF_FPU_STATE]
    test rax, rax
    jz .skip_fxrstor
    fxrstor64 [rax]
.skip_fxrstor:

    ; 5. Cargar nuevo RSP
    mov rsp, [rsi + TASK_OFF_RSP]

    ; 5b. [FIX] Ahora estamos en el stack de la nueva tarea. Marcar
    ; on_cpu de la vieja = 0 y de la nueva = 1. Este es el punto
    ; seguro: ya no volveremos a tocar el stack viejo, así que otra
    ; CPU puede reapear `old` sin que nosotros usemos su memoria.
    mov dword [rdi + TASK_OFF_ON_CPU], 0
    mov dword [rsi + TASK_OFF_ON_CPU], 1

    ; 6. Cambiar CR3 si hace falta
    mov rax, [rsi + TASK_OFF_CR3]
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
