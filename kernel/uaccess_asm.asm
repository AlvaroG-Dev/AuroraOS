; kernel/uaccess_asm.asm
;
; Primitivas de uaccess con soporte de fixup.
;
; Cada símbolo `..._fault` marca la dirección de la instrucción que
; puede fallar. La tabla de fixups (uaccess_faults.asm) asocia cada
; `..._fault` con el `..._fixup` correspondiente.
;
[BITS 64]

section .text

; ---------------------------------------------------------------------------
; long raw_cmpxchg_user_u32(uint32_t *uaddr, uint32_t old, uint32_t new,
;                           uint32_t *cur)
; CAS atomico sobre memoria de usuario (futex PI). *cur = valor que habia.
; 0 = hecho (mirar *cur == old para saber si se cambio), -EFAULT si falla.
; ---------------------------------------------------------------------------
global raw_cmpxchg_user_u32
global raw_cmpxchg_user_u32_fault
global raw_cmpxchg_user_u32_fixup

raw_cmpxchg_user_u32:
    mov eax, esi
    stac
raw_cmpxchg_user_u32_fault:
    lock cmpxchg [rdi], edx
    clac
    mov [rcx], eax
    xor eax, eax
    ret

raw_cmpxchg_user_u32_fixup:
    clac
    mov rax, -14
    ret

; ---------------------------------------------------------------------------
; long raw_copy_from_user(void *dst, const void *src, size_t n)
; ---------------------------------------------------------------------------
global raw_copy_from_user
global raw_copy_from_user_fault
global raw_copy_from_user_fixup

raw_copy_from_user:
    push rdi
    push rsi
    push rcx
    push rdx
    mov rcx, rdx
    stac
raw_copy_from_user_fault:
    rep movsb
    clac
    xor rax, rax
    pop rdx
    pop rcx
    pop rsi
    pop rdi
    ret

raw_copy_from_user_fixup:
    clac
    mov rax, rcx
    pop rdx
    pop rcx
    pop rsi
    pop rdi
    ret

; ---------------------------------------------------------------------------
; long raw_copy_to_user(void *dst, const void *src, size_t n)
; ---------------------------------------------------------------------------
global raw_copy_to_user
global raw_copy_to_user_fault
global raw_copy_to_user_fixup

raw_copy_to_user:
    push rdi
    push rsi
    push rcx
    push rdx
    mov rcx, rdx
    stac
raw_copy_to_user_fault:
    rep movsb
    clac
    xor rax, rax
    pop rdx
    pop rcx
    pop rsi
    pop rdi
    ret

raw_copy_to_user_fixup:
    clac
    mov rax, rcx
    pop rdx
    pop rcx
    pop rsi
    pop rdi
    ret

; ---------------------------------------------------------------------------
; long raw_strncpy_from_user(char *dst, const char *src, size_t max)
; ---------------------------------------------------------------------------
global raw_strncpy_from_user
global raw_strncpy_from_user_fault
global raw_strncpy_from_user_fixup
global raw_strncpy_from_user_loop
global raw_strncpy_from_user_done
global raw_strncpy_from_user_empty

raw_strncpy_from_user:
    push rdi
    push rsi
    push rbx
    mov rbx, rdx            ; rbx = max
    test rbx, rbx
    jz raw_strncpy_from_user_empty
    xor rax, rax            ; i = 0
    dec rbx                 ; rbx = max-1
    stac
raw_strncpy_from_user_loop:
raw_strncpy_from_user_fault:
    mov dl, [rsi + rax]
    test dl, dl
    jz raw_strncpy_from_user_done
    mov [rdi + rax], dl
    inc rax
    cmp rax, rbx
    jb raw_strncpy_from_user_loop
raw_strncpy_from_user_done:
    mov byte [rdi + rax], 0
    clac
    pop rbx
    pop rsi
    pop rdi
    ret
raw_strncpy_from_user_empty:
    clac
    mov byte [rdi], 0
    xor rax, rax
    pop rbx
    pop rsi
    pop rdi
    ret

raw_strncpy_from_user_fixup:
    clac
    mov rax, -14            ; -EFAULT
    pop rbx
    pop rsi
    pop rdi
    ret

section .note.GNU-stack noalloc noexec nowrite progbits