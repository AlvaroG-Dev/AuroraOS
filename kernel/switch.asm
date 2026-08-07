; kernel/switch.asm
; Context switch para el scheduler de Aurora OS
;
; void task_switch(task_t *old_task, task_t *new_task);
;   rdi = old_task
;   rsi = new_task
;
; Solo guardamos/restauramos los registros CALLEE-SAVED (ABI System V x86-64):
;   rbx, rbp, r12, r13, r14, r15
; El compilador ya se encargo de guardar los caller-saved antes de llamarnos.

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

    ; Guardar el RSP actual en old_task->rsp (offset 0 del struct)
    mov [rdi], rsp

    ; 2. Cargar el puntero FPU alineado (offset 24 / 0x18) y guardar estado
    mov rax, [rdi + 24]
    fxsave64 [rax]

    ; 3. Cargar el puntero FPU alineado de la nueva tarea y restaurar estado
    mov rax, [rsi + 24]
    fxrstor64 [rax]

    ; --- Restaurar contexto de la tarea nueva (new_task = rsi) ---
    ; Cargar RSP desde new_task->rsp (offset 0 del struct)
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; El 'ret' salta a donde new_task se quedó la última vez
    ; (o a task_trampoline si es la primera vez que corre)
    ret

; ---------------------------------------------------------------------------
; task_trampoline — punto de entrada inicial de toda nueva tarea kernel.
; Cuando task_switch hace 'ret' por primera vez en una tarea recien creada,
; el stack tiene la direccion de task_trampoline. r12 contiene el puntero
; a la funcion de la tarea (fn), puesto ahi por sched_create_task.
; Movemos r12 -> rdi para cumplir la ABI System V y llamamos al wrapper C.
; ---------------------------------------------------------------------------
global task_trampoline
extern task_entry_wrapper

task_trampoline:
    sti                 ; Habilitar interrupciones: las tareas nuevas entran con IF=0
                        ; (venimos de un handler IRQ). Necesario para que hlt funcione.
    mov rdi, r12        ; fn esta en r12 (guardado en stack por sched_create_task)
    call task_entry_wrapper
    ; Si task_entry_wrapper retorna (no deberia), loop infinito
.hang:
    hlt
    jmp .hang
