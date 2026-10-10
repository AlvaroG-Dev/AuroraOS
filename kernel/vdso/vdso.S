// kernel/vdso/vdso.S
//
// vDSO para Aurora OS. Funciones en userspace que glibc usa en
// lugar de la syscall si las encuentra en el ELF mapeado en
// AT_SYSINFO_EHDR:
//
//   __vdso_clock_gettime  (CLOCK_MONOTONIC=1, CLOCK_REALTIME=0)
//   __vdso_gettimeofday
//   __vdso_time
//   __vdso_getcpu
//
// Ambas direcciones (vvar y vdso) son FIJAS: sin ASLR. Por eso
// el codigo puede usar movabs con el literal de VVAR_VMA_BASE
// en vez de RIP-relative. Si algun dia se anade ASLR, habra que
// reescribir esto con relocations aplicadas por el kernel.
//
// Layout de vvar (offsets hardcoded):
//   +0x00  uint32 seq         (seqlock par/impar)
//   +0x04  uint32 clock_mode  (0 = no disponible, 1 = activo)
//   +0x08  uint64 mono_ns
//   +0x10  uint64 real_ns

    .text

// ===========================================================================
// int __vdso_clock_gettime(clockid_t clockid, struct timespec *tp)
//   rdi = clockid, rsi = tp
// Devuelve 0 si OK, o cae al syscall para clockids no soportados
// o si el vvar no esta activo.
// ===========================================================================
    .globl __vdso_clock_gettime
    .type __vdso_clock_gettime, @function
    .p2align 4
__vdso_clock_gettime:
    movabs  $0x00007FFFFFFF0000, %r8
    cmpl    $0, 4(%r8)                  // clock_mode
    je      .Lcgt_fallback
    cmp     $1, %edi
    ja      .Lcgt_fallback

.Lcgt_retry:
    mov     (%r8), %eax                 // seq
    test    $1, %eax
    jnz     .Lcgt_retry
    mov     8(%r8), %rcx                // mono_ns
    mov     16(%r8), %rdx               // real_ns
    cmp     (%r8), %eax
    jne     .Lcgt_retry

    test    %edi, %edi
    jnz     .Lcgt_have_ts
    mov     %rdx, %rax                  // CLOCK_REALTIME
    jmp     .Lcgt_split
.Lcgt_have_ts:
    mov     %rcx, %rax                  // CLOCK_MONOTONIC
.Lcgt_split:
    xor     %edx, %edx
    mov     $1000000000, %rcx
    div     %rcx
    mov     %rax, (%rsi)                // tv_sec
    mov     %rdx, 8(%rsi)               // tv_nsec
    xor     %eax, %eax
    ret

.Lcgt_fallback:
    mov     $228, %eax                  // SYS_clock_gettime
    syscall
    ret
    .size __vdso_clock_gettime, .-__vdso_clock_gettime

// ===========================================================================
// int __vdso_gettimeofday(struct timeval *tv, void *tz)
//   rdi = tv, rsi = tz
// ===========================================================================
    .globl __vdso_gettimeofday
    .type __vdso_gettimeofday, @function
    .p2align 4
__vdso_gettimeofday:
    movabs  $0x00007FFFFFFF0000, %r8
    cmpl    $0, 4(%r8)
    je      .Lgtod_syscall

    test    %rdi, %rdi
    jz      .Lgtod_tz

.Lgtod_retry:
    mov     (%r8), %eax
    test    $1, %eax
    jnz     .Lgtod_retry
    mov     16(%r8), %rcx               // real_ns
    cmp     (%r8), %eax
    jne     .Lgtod_retry

    mov     %rcx, %rax
    xor     %edx, %edx
    mov     $1000000000, %rcx
    div     %rcx
    mov     %rax, (%rdi)                // tv_sec
    mov     %rdx, %rax
    xor     %edx, %edx
    mov     $1000, %ecx
    div     %rcx
    mov     %rax, 8(%rdi)               // tv_usec = nsec / 1000

.Lgtod_tz:
    test    %rsi, %rsi
    jz      .Lgtod_ok
    movq    $0, (%rsi)
    movq    $0, 8(%rsi)
.Lgtod_ok:
    xor     %eax, %eax
    ret

.Lgtod_syscall:
    mov     $96, %eax                   // SYS_gettimeofday
    syscall
    ret
    .size __vdso_gettimeofday, .-__vdso_gettimeofday

// ===========================================================================
// time_t __vdso_time(time_t *tloc)
//   rdi = tloc (o NULL)
// ===========================================================================
    .globl __vdso_time
    .type __vdso_time, @function
    .p2align 4
__vdso_time:
    movabs  $0x00007FFFFFFF0000, %r8
    cmpl    $0, 4(%r8)
    je      .Ltime_syscall

.Ltime_retry:
    mov     (%r8), %eax
    test    $1, %eax
    jnz     .Ltime_retry
    mov     16(%r8), %rcx
    cmp     (%r8), %eax
    jne     .Ltime_retry

    mov     %rcx, %rax
    xor     %edx, %edx
    mov     $1000000000, %rcx
    div     %rcx
    test    %rdi, %rdi
    jz      .Ltime_ok
    mov     %rax, (%rdi)
.Ltime_ok:
    ret

.Ltime_syscall:
    mov     $201, %eax                  // SYS_time
    syscall
    ret
    .size __vdso_time, .-__vdso_time

// ===========================================================================
// int __vdso_getcpu(unsigned *cpu, unsigned *node, void *tcache)
// Stub: escribe 0, devuelve 0. Sin campo en vvar todavia.
// ===========================================================================
    .globl __vdso_getcpu
    .type __vdso_getcpu, @function
    .p2align 4
__vdso_getcpu:
    test    %rdi, %rdi
    jz      .Lgetcpu_node
    movl    $0, (%rdi)
.Lgetcpu_node:
    test    %rsi, %rsi
    jz      .Lgetcpu_ok
    movl    $0, (%rsi)
.Lgetcpu_ok:
    xor     %eax, %eax
    ret
    .size __vdso_getcpu, .-__vdso_getcpu

    .section .note.GNU-stack, "", @progbits