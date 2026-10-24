// kernel/kthread.h
#ifndef KERNEL_KTHREAD_H
#define KERNEL_KTHREAD_H

#include "sched.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Kernel threads.
//
// Un kthread es una task_t normal cuyo entry point es un trampolín que llama
// a fn(data). La fn debe devolver int (el exit code) o entrar en un bucle
// consultando kthread_should_stop() y salir cuando devuelva 1.
//
// Ciclo de vida:
//
//   struct kthread *kt = kthread_run(fn, data, "miworker");
//   ...
//   int code = kthread_stop(kt);   // bloquea, espera, libera kt
//
//   // Si quieres separar creación y publicación:
//   struct kthread *kt = kthread_create(fn, data, "miworker");
//   ...  (aquí puedes preconfigurar el task, ej. afinidad)
//   kthread_start(kt);
//
// Reglas:
//   - kthread_create/kthread_run deben llamarse en process context
//     (sched_current() != NULL). Típicamente desde kmain_task o desde
//     otra tarea de kernel.
//   - kthread_stop debe llamarse exactamente una vez por kthread y
//     libera el handle. Si nunca se llama, el handle y su task_t quedan
//     (pequeña) memoria perdida; el task_t no se reapea porque sigue
//     teniendo la ref del handle.
//   - fn NO debe llamar kthread_stop sobre sí mismo.
// ---------------------------------------------------------------------------

struct kthread; // handle opaco

// Crea un kthread sin publicarlo. Devuelve NULL si falla la reserva de
// memoria o el stack. `name` se trunca a 15 chars; si es NULL/vacío se usa
// "kthread".
struct kthread *kthread_create(int (*fn)(void *), void *data, const char *name);

// Publica el kthread. A partir de aquí puede ejecutarse en cualquier CPU.
// Debe llamarse exactamente una vez. Devuelve 0 o -EINVAL.
int kthread_start(struct kthread *kt);

// Conveniencia: kthread_create + kthread_start. Devuelve NULL en error.
struct kthread *kthread_run(int (*fn)(void *), void *data, const char *name);

// Pide al kthread que pare, lo despierta si está bloqueado, espera a que
// termine (state == DEAD y on_cpu == 0), y libera el handle.
// Devuelve el exit code de la fn (o -EINVAL si kt es NULL).
int kthread_stop(struct kthread *kt);

// Consulta desde dentro de un kthread si kthread_stop() está pendiente.
// El bucle del kthread debe consultarlo periódicamente y retornar.
int kthread_should_stop(void);

// Acceso al task_t subyacente. Solo válido hasta kthread_stop().
task_t *kthread_task(struct kthread *kt);

// Devuelve el nombre del kthread (string estático del handle).
const char *kthread_name(struct kthread *kt);

// Si la tarea actual es un kthread, devuelve su handle; si no, NULL.
struct kthread *kthread_current(void);

// Termina el kthread actual sin retornar. Poco común; normalmente la fn
// simplemente retorna su código de salida.
__attribute__((noreturn)) void kthread_exit(int code);

#endif