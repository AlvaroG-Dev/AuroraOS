; kernel/switch.asm
;
; TODO SMP (Fase 4): cuando SMP esté activo, este archivo tendrá que
; gestionar el %gs: per-CPU. Hoy asume single-CPU: el %gs: base es
; siempre &cpu_local del BSP.
;
; Los cambios serán:
;   - Al cambiar de tarea, si la nueva tarea corre en otra CPU, cargar
;     su %gs: base (o dejarlo si es la misma CPU).
;   - Los accesos a cpu_local vía [rel cpu_local + offset] pasarán a
;     %gs:offset.
;
; Context switch + trampoline para Aurora OS
;
; task_t offsets (ver sched.h):
;   0x00  rsp        (uint64_t)
;   0x08  stack      (uint64_t*)
;   0x10  id         (uint32_t)
;   0x14  state      (task_state_t / uint32_t)
;   0x18  fpu_state  (uint64_t)  <- puntero alineado al buffer FPU
;   0x20  cr3        (uint64_t)  <- CR3 fisico del espacio de direcciones
;   0x28  fpu_raw    (uint8_t[528])
;   0x238 next       (task_t*)

section .text
bits 64

global task_switch

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

    ; 3. Restaurar FPU/SSE del nuevo proceso (offset 0x18)
    mov rax, [rsi + 0x18]
    test rax, rax
    jz .skip_fxrstor
    fxrstor64 [rax]
.skip_fxrstor:

    ; 4. Cargar nuevo RSP desde new_task->rsp (offset 0x00)
    mov rsp, [rsi]

    ; 5. Cambiar espacio de direcciones: cargar CR3 de new_task (offset 0x20)
    ;    Solo escribir si cambia para evitar flush de TLB innecesario
    mov rax, [rsi + 0x20]
    test rax, rax           ; Si CR3 == 0, no cambiar (tarea huerfana/idle sin pml4)
    jz .skip_cr3
    mov rdx, cr3
    cmp rax, rdx
    je .skip_cr3
    mov cr3, rax
.skip_cr3:

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; ret salta al punto donde la nueva tarea fue interrumpida
    ; (o a task_trampoline/user_trampoline si es la primera vez)
    ret

; ---------------------------------------------------------------------------
; task_jump_to(task_t *task)
;
; Carga el contexto de `task` y salta a él. NO guarda el contexto actual.
;
; Se usa UNA SOLA VEZ, desde el arranque, para saltar de "kmain corriendo
; en el stack del bootloader" a "kmain_task corriendo en su propio stack".
; Después de esto, el scheduler usa task_switch (que sí guarda).
;
; Argumento: RDI = task_t *
;
; El stack de la tarea debe tener el layout armado por sched_create_task:
;   [rsp+0]  = r15
;   [rsp+8]  = r14
;   [rsp+16] = r13
;   [rsp+24] = r12 (fn)
;   [rsp+32] = rbp
;   [rsp+40] = rbx
;   [rsp+48] = ret address (task_trampoline o user_trampoline)
; ---------------------------------------------------------------------------
global task_jump_to
task_jump_to:
    ; Restaurar FPU/SSE de la nueva tarea.
    mov rax, [rdi + 0x18]
    test rax, rax
    jz .skip_fxrstor
    fxrstor64 [rax]
.skip_fxrstor:

    ; Cargar el RSP de la tarea nueva.
    mov rsp, [rdi]

    ; Cambiar CR3 si hace falta.
    mov rax, [rdi + 0x20]
    test rax, rax
    jz .skip_cr3
    mov rdx, cr3
    cmp rax, rdx
    je .skip_cr3
    mov cr3, rax
.skip_cr3:

    ; Restaurar los registros callee-saved. Orden inverso a los pushes
    ; de sched_create_task.
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; ret salta a task_trampoline o user_trampoline (el primer ret address
    ; del stack de la tarea).
    ret

; ---------------------------------------------------------------------------
; task_trampoline: punto de entrada inicial de tareas de kernel.
; r12 contiene el puntero a la funcion puesto por sched_create_task.
; ---------------------------------------------------------------------------
global task_trampoline
extern task_entry_wrapper

task_trampoline:
    sti                     ; Habilitar interrupciones (venimos de handler IRQ)
    mov rdi, r12            ; fn -> primer argumento SysV
    call task_entry_wrapper
.hang:
    hlt
    jmp .hang

; user_trampoline: ejecutado en Ring 0 justo antes de saltar a Ring 3.
; El kernel stack tiene el frame IRETQ sintetico creado por
; sched_create_user_task(). Cargamos selectores de usuario y hacemos IRETQ.
; ---------------------------------------------------------------------------
global user_trampoline

user_trampoline:
    ; Cargar selector de segmento de datos de usuario (USER_DS | 3 = 0x1B)
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    ; FS y GS se dejan en 0 (base controlada por MSR en 64 bits)

    ; Limpiar todos los registros de proposito general antes de entrar a Ring 3
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