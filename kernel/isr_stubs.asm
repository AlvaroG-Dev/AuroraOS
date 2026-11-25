; kernel/isr_stubs.asm
; Stubs de ISR/IRQ que empujan estado y llaman a handlers en C.
;
; swapgs: en userland %gs lleva la base del usuario (ARCH_SET_GS, el TEB de
; Wine) y la del kernel (cpu_local_data) espera en MSR_KERNEL_GS_BASE. Las
; IRQ/MSI/IPI miran el CS guardado (de ring 3 -> swapgs al entrar y al salir).
; Las excepciones (isr_common) son "paranoicas": leen MSR_GS_BASE, porque un
; NMI/#MC/#DB puede llegar en el kernel justo antes del swapgs de la entrada
; de syscall o tras el de la salida.
;
; Alineación ABI System V: antes de cada CALL, RSP debe ser ≡ 0 (mod 16).
; Con 15 pushes queda ≡ 8; añadimos un push dummy al final (16 pushes)
; y pasamos al handler RDI = RSP + 8 (saltando el dummy) para que el
; layout de registers_t sea el esperado por el código C.

section .text
bits 64

; ---------------------------------------------------------------------------
; Estado FPU/SSE/AVX del contexto interrumpido.
;
; El kernel se compila con SSE (gcc usa XMM para poner structs a cero, copiar,
; etc.), asi que cualquier entrada al kernel pisaba los XMM del usuario (o del
; propio kernel interrumpido). Cada entrada guarda el estado con xsave (o
; fxsave si no hay XSAVE) en la pila y lo restaura antes de volver.
;
;   FPU_SAVE    r12 = rsp al entrar (frame base, callee-saved: sobrevive a la
;               llamada C y a task_switch). Deja rsp alineado a 64 apuntando
;               al area. Clobbera rax, rdx (ya salvados en el frame).
;   FPU_RESTORE vuelve a rsp = r12.
;
; fpu_use_xsave / fpu_xsave_lo/hi / fpu_area_size los fija cpu_simd_init()
; con las interrupciones cortadas, antes de que haya ningun frame vivo.
; ---------------------------------------------------------------------------
extern fpu_use_xsave
extern fpu_xsave_lo
extern fpu_xsave_hi
extern fpu_area_size

%macro FPU_SAVE 0
    mov r12, rsp
    mov eax, [rel fpu_area_size]
    sub rsp, rax
    and rsp, -64
    cmp dword [rel fpu_use_xsave], 0
    je %%fx
    ; Cabecera XSAVE entera a cero: xsave conserva los bits de XSTATE_BV fuera de RFBM
    ; (basura de la pila) y xrstor da #GP si hay bits fuera de XCR0 o xcomp_bv != 0.
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
    jmp %%done
%%fx:
    fxsave64 [rsp]
%%done:
    ldmxcsr [rel kernel_mxcsr]      ; el kernel corre con MXCSR por omision
%endmacro

%macro FPU_RESTORE 0
    cmp dword [rel fpu_use_xsave], 0
    je %%fx
    mov eax, [rel fpu_xsave_lo]
    mov edx, [rel fpu_xsave_hi]
    xrstor64 [rsp]
    jmp %%done
%%fx:
    fxrstor64 [rsp]
%%done:
    mov rsp, r12
%endmacro

section .rodata
align 4
kernel_mxcsr: dd 0x1F80
section .text

%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0          ; dummy error code
    push %1         ; ISR number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push %1         ; ISR number (error code ya esta en stack)
    jmp isr_common
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    push 0
    push %2
    jmp irq_common
%endmacro

; int 0x80: syscalls de 32 bits (Wine WoW64 pide así su %fs con
; set_thread_area, como en Linux).
ISR_NOERR 128

; Excepciones CPU (0-31)
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; IRQs del IOAPIC (0-15 -> IDT 32-47)
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; LAPIC timer (vector 48).
global irq48
irq48:
    push 0
    push 48
    jmp irq_common

extern isr_handler
extern irq_handler

; ---------------------------------------------------------------------------
; isr_common
; ---------------------------------------------------------------------------
isr_common:
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

    ; Paranoico: ¿%gs ya es la del kernel (dirección alta)? Si no, swapgs.
    ; ebx (callee-saved, se conserva en isr_handler) recuerda si hubo que
    ; hacerlo para deshacerlo a la salida.
    mov ecx, 0xC0000101              ; MSR_GS_BASE
    rdmsr
    xor ebx, ebx
    test edx, edx
    js .gs_kernel
    swapgs
    mov ebx, 1
.gs_kernel:
    push r15                         ; slot: puntero al area FPU

    FPU_SAVE
    mov [r12], rsp                   ; slot [regs-8] = area FPU (ver signal/fork)
    lea rdi, [r12 + 8]               ; registers_t* (saltando el slot)
    call isr_handler
    FPU_RESTORE

    add rsp, 8                       ; quitar slot
    test ebx, ebx
    jz .gs_keep
    swapgs
.gs_keep:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16                      ; limpiar int_num + error_code
    iretq

; ---------------------------------------------------------------------------
; irq_common
; ---------------------------------------------------------------------------
irq_common:
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
    test qword [rsp + 144], 3        ; CS guardado: ¿venía de ring 3?
    jz .irq_common_k_in
    swapgs
.irq_common_k_in:
    push r15                         ; slot: puntero al area FPU

    FPU_SAVE
    mov [r12], rsp                   ; slot [regs-8] = area FPU (ver signal/fork)
    lea rdi, [r12 + 8]               ; registers_t* (saltando el slot)
    call irq_handler
    FPU_RESTORE

    add rsp, 8                       ; quitar slot
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16                      ; limpiar int_num + error_code
    test qword [rsp + 8], 3          ; CS al que volvemos
    jz .irq_common_k_out
    swapgs
.irq_common_k_out:
    iretq


; ---------------------------------------------------------------------------
; [SMP 4.4] Stubs de IPI. Vectores 0xFB (resched), 0xFC (TLB).
;
; En kernel-to-kernel IPI no hay cambio de privilegio. El hardware no
; empuja error_code para estos vectores, así que empujamos un dummy.
; ---------------------------------------------------------------------------
extern ipi_handler_resched
extern ipi_handler_tlb

global ipi_stub_resched
ipi_stub_resched:
    push 0
    push 0xFB
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    test qword [rsp + 96], 3         ; CS guardado (9 regs + 2 = 88 bytes, +8 rip)
    jz .ipi_stub_resched_k_in
    swapgs
.ipi_stub_resched_k_in:
    push r12
    FPU_SAVE
    call ipi_handler_resched
    FPU_RESTORE
    pop r12
    test qword [rsp + 96], 3
    jz .ipi_stub_resched_k_out
    swapgs
.ipi_stub_resched_k_out:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

; IPI_VECTOR_HALT (0xFE): parar este CPU para siempre (apagado/reinicio).
global ipi_stub_halt
ipi_stub_halt:
    cli
.hang:
    hlt
    jmp .hang

global ipi_stub_tlb
ipi_stub_tlb:
    push 0
    push 0xFC
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    test qword [rsp + 96], 3         ; CS guardado (9 regs + 2 = 88 bytes, +8 rip)
    jz .ipi_stub_tlb_k_in
    swapgs
.ipi_stub_tlb_k_in:
    push r12
    FPU_SAVE
    call ipi_handler_tlb
    FPU_RESTORE
    pop r12
    test qword [rsp + 96], 3
    jz .ipi_stub_tlb_k_out
    swapgs
.ipi_stub_tlb_k_out:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq
    

; ---------------------------------------------------------------------------
; MSI stubs. Un stub por vector (0x60-0x7F).
;
; El CPU usa el vector MSI como índice en la IDT, y el stub es lo único
; que puede decirle al handler C qué vector disparó. Por eso hay uno por
; vector y no uno genérico: cuando el e1000e dispara el 0x61, el stub
; empuja 0x61; cuando el AHCI dispara el 0x60, empuja 0x60.
;
; Si falta la IDT entry de un vector que está en uso (bug que teníamos),
; el CPU lanza #GP con error_code = 0x30X, donde X = vector.
; ---------------------------------------------------------------------------
extern msi_handler

%macro MSI_STUB 1
global msi_stub_%1
msi_stub_%1:
    push 0          ; dummy error code
    push %1         ; int_num = vector
    jmp msi_common
%endmacro

MSI_STUB 0x60
MSI_STUB 0x61
MSI_STUB 0x62
MSI_STUB 0x63
MSI_STUB 0x64
MSI_STUB 0x65
MSI_STUB 0x66
MSI_STUB 0x67
MSI_STUB 0x68
MSI_STUB 0x69
MSI_STUB 0x6A
MSI_STUB 0x6B
MSI_STUB 0x6C
MSI_STUB 0x6D
MSI_STUB 0x6E
MSI_STUB 0x6F
MSI_STUB 0x70
MSI_STUB 0x71
MSI_STUB 0x72
MSI_STUB 0x73
MSI_STUB 0x74
MSI_STUB 0x75
MSI_STUB 0x76
MSI_STUB 0x77
MSI_STUB 0x78
MSI_STUB 0x79
MSI_STUB 0x7A
MSI_STUB 0x7B
MSI_STUB 0x7C
MSI_STUB 0x7D
MSI_STUB 0x7E
MSI_STUB 0x7F

; ---------------------------------------------------------------------------
; msi_common: como irq_common, pero llama a msi_handler en vez de irq_handler.
; ---------------------------------------------------------------------------
msi_common:
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
    test qword [rsp + 144], 3        ; CS guardado: ¿venía de ring 3?
    jz .msi_common_k_in
    swapgs
.msi_common_k_in:
    push r15                         ; slot: puntero al area FPU

    FPU_SAVE
    mov [r12], rsp                   ; slot [regs-8] = area FPU (ver signal/fork)
    lea rdi, [r12 + 8]               ; registers_t* (saltando el slot)
    call msi_handler
    FPU_RESTORE

    add rsp, 8                       ; quitar slot
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16                      ; limpiar int_num + error_code
    test qword [rsp + 8], 3          ; CS al que volvemos
    jz .msi_common_k_out
    swapgs
.msi_common_k_out:
    iretq

; ---------------------------------------------------------------------------
; isr_spurious: stub para el vector espurio del LAPIC (0xFF).
; No hace nada. Solo iretq. Configurado en la IDT para que el LAPIC
; no cause un triple fault si entrega una interrupción espuria.
; ---------------------------------------------------------------------------
global isr_spurious
isr_spurious:
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits