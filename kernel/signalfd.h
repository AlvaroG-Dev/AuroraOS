// kernel/signalfd.h
#ifndef KERNEL_SIGNALFD_H
#define KERNEL_SIGNALFD_H

#include <stdint.h>

// syscalls 282 y 289.
int64_t k_signalfd(uint64_t fd, uint64_t mask_uptr, uint64_t sizemask,
                   uint64_t a4, uint64_t a5);
int64_t k_signalfd4(uint64_t fd, uint64_t mask_uptr, uint64_t sizemask,
                    uint64_t flags, uint64_t a5);

// [signalfd] Se llama desde signal.c cuando se encola una señal al
// proceso. Despierta los signalfds del proceso cuyas máscaras
// intersecten con sig_mask.
struct process;
void signalfd_notify(struct process *proc, uint64_t sig_mask);

// [signalfd] Se llama desde process_exit. Desbloquea las señales que
// cada signalfd había bloqueado y libera la lista.
struct process;
void signalfd_cleanup(struct process *proc);

#endif