; kernel/isr_stubs.asm
; Stubs de ISR/IRQ que empujan estado y llaman a handlers en C.
;
; No usamos swapgs ni GS. El acceso a cpu_local se hace por dirección
; absoluta / RIP-relative (kernel mapeado en todas las tareas).
;
; Alineación ABI System V: antes de cada CALL, RSP debe ser ≡ 0 (mod 16).
; Con 15 pushes queda ≡ 8; añadimos un push dummy al final (16 pushes)
; y pasamos al handler RDI = RSP + 8 (saltando el dummy) para que el
; layout de registers_t sea el esperado por el código C.

section .text
bits 64

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
    push r15                         ; dummy: 16 pushes → alineación a 16

    mov rdi, rsp
    add rdi, 8                       ; saltar el dummy
    call isr_handler

    add rsp, 8                       ; quitar dummy
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
    push r15                         ; dummy: 16 pushes → alineación a 16

    mov rdi, rsp
    add rdi, 8                       ; saltar el dummy
    call irq_handler

    add rsp, 8                       ; quitar dummy
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
    call ipi_handler_resched
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
    call ipi_handler_tlb
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
; MSI stubs. Un solo vector (0x60) para el AHCI.
;
; Igual que irq_common, pero con int_num hardcodeado a 0x60.
; Lo usamos en lugar de una macro porque solo necesitamos uno.
; ---------------------------------------------------------------------------
extern msi_handler

global msi_stub_0x60
msi_stub_0x60:
    push 0          ; dummy error code
    push 0x60       ; int_num = 0x60
    jmp msi_common

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
    push r15                         ; dummy: alineación a 16

    mov rdi, rsp
    add rdi, 8                       ; saltar el dummy
    call msi_handler

    add rsp, 8                       ; quitar dummy
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
; isr_spurious: stub para el vector espurio del LAPIC (0xFF).
; No hace nada. Solo iretq. Configurado en la IDT para que el LAPIC
; no cause un triple fault si entrega una interrupción espuria.
; ---------------------------------------------------------------------------
global isr_spurious
isr_spurious:
    iretq