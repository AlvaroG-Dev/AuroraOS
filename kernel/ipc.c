// kernel/ipc.c
//
// IPC por paso de mensajes.
//
// IMPORTANTE: este módulo SOLO acepta punteros del kernel. El camino
// de syscall (sys_ipc_send_k / sys_ipc_recv_k) hace copy_from_user /
// copy_to_user ANTES/DESPUÉS de llamar aquí. Así:
//   - No hay stac/clac en este archivo (el bug anterior: si memcpy
//     faultaba, quedabas con AC=1 y el #PF no tenía fixup).
//   - El código de IPC es agnóstico de si el llamante es kernel o
//     userland.
//   - Los tests del kernel pueden usar punteros de kernel directamente.
//
// El lock del mailbox ES el lock de la wait queue. Así la condición
// (count > 0) y la modificación de count están protegidas por el mismo
// lock, eliminando la race clásica entre productor y consumidor.

#include "ipc.h"
#include "cpu.h"
#include "klog.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "wait.h"
#include "heap.h"

// Devuelve una referencia propia al task mientras sched_lock está protegido.
extern task_t *sched_find_task_get(uint32_t task_id);

void ipc_init(void) {
  LOG_INFO("[IPC] Subsistema de Paso de Mensajes inicializado.");
}

// ---------------------------------------------------------------------------
// El buzon de una tarea se crea la primera vez que se usa (casi ninguna
// tarea hace IPC). Dos que lo creen a la vez: gana uno (cmpxchg) y el otro
// libera el suyo. Vive hasta que se libera la tarea (task_put).
// ---------------------------------------------------------------------------
static ipc_mailbox_t *mailbox_of(task_t *t) {
  ipc_mailbox_t *mb = __atomic_load_n(&t->mailbox, __ATOMIC_ACQUIRE);
  if (mb)
    return mb;
  ipc_mailbox_t *fresh = (ipc_mailbox_t *)kzalloc(sizeof(*fresh));
  if (!fresh)
    return NULL;
  wait_queue_init(&fresh->wq);
  ipc_mailbox_t *expected = NULL;
  if (__atomic_compare_exchange_n(&t->mailbox, &expected, fresh, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return fresh;
  kfree(fresh);
  return expected;
}

// ---------------------------------------------------------------------------
// Helpers de lock del mailbox.
// ---------------------------------------------------------------------------
static unsigned long mailbox_lock(ipc_mailbox_t *mb) {
  return spin_lock_irqsave(&mb->wq.lock);
}

static void mailbox_unlock(ipc_mailbox_t *mb, unsigned long flags) {
  spin_unlock_irqrestore(&mb->wq.lock, flags);
}

// ---------------------------------------------------------------------------
// ipc_send: envía un mensaje al mailbox de `target_id`.
//
// `data` es un puntero DEL KERNEL. El llamante (syscall) debe haber
// copiado previamente desde userland con copy_from_user si procede.
//
// Retorna:
//   0  OK
//  -1  target no existe / current no existe
//  -2  mailbox lleno
// ---------------------------------------------------------------------------
int ipc_send(uint32_t target_id, uint32_t type, const void *data, size_t size) {
  task_t *current = sched_current();
  if (!current)
    return -1;

  task_t *target = sched_find_task_get(target_id);
  if (!target)
    return -1;

  // Copiar payload a un buffer local ANTES de coger el lock.
  // `data` ya es del kernel, así que memcpy directo es seguro.
  uint8_t tmp[IPC_MAX_PAYLOAD];
  size_t copy_len = size > IPC_MAX_PAYLOAD ? IPC_MAX_PAYLOAD : size;
  if (data && copy_len > 0) {
    memcpy(tmp, data, copy_len);
  }

  ipc_mailbox_t *mb = mailbox_of(target);
  if (!mb) {
    task_put(target);
    return -2;
  }
  unsigned long flags = mailbox_lock(mb);

  if (mb->count >= IPC_MAILBOX_SIZE) {
    mailbox_unlock(mb, flags);
    task_put(target);
    return -2;
  }

  uint8_t tail = mb->tail;
  ipc_msg_t *msg = &mb->messages[tail];

  msg->sender = current->id;
  msg->receiver = target_id;
  msg->type = type;
  msg->size = (uint32_t)copy_len;
  if (copy_len > 0) {
    memcpy(msg->data, tmp, copy_len);
  }

  mb->tail = (tail + 1) % IPC_MAILBOX_SIZE;
  mb->count++;

  // Atómico con count++: el lock ya está cogido.
  wake_up_all_locked(&mb->wq);

  mailbox_unlock(mb, flags);
  task_put(target);
  return 0;
}

// ---------------------------------------------------------------------------
// Condición de la wait queue del mailbox.
// Se llama con el lock del mailbox (= lock de la wq) cogido.
// ---------------------------------------------------------------------------
static bool ipc_mailbox_has_message(void *arg) {
  ipc_mailbox_t *mb = (ipc_mailbox_t *)arg;
  return mb->count > 0;
}

// ---------------------------------------------------------------------------
// ipc_recv: recibe un mensaje del mailbox del task actual.
//
// `out_msg` es un puntero DEL KERNEL. El llamante (syscall) debe
// copiarlo a userland con copy_to_user después.
//
// Retorna:
//   0  OK
//  -1  no hay mensaje (non_blocking) o current no existe
//  -EINTR si wait_event_interruptible fue interrumpido
// ---------------------------------------------------------------------------
int ipc_recv(ipc_msg_t *out_msg, int non_blocking) {
  task_t *current = sched_current();
  if (!current || !out_msg)
    return -1;
  ipc_mailbox_t *mb = mailbox_of(current);
  if (!mb)
    return -1;

  if (non_blocking) {
    unsigned long flags = mailbox_lock(mb);
    if (mb->count == 0) {
      mailbox_unlock(mb, flags);
      return -1;
    }
    uint8_t head = mb->head;
    // Copiamos a un buffer local para no exponer el mailbox
    // mientras tenemos el lock. Es del kernel, memcpy directo.
    ipc_msg_t tmp;
    memcpy(&tmp, &mb->messages[head], sizeof(ipc_msg_t));
    mb->head = (head + 1) % IPC_MAILBOX_SIZE;
    mb->count--;
    mailbox_unlock(mb, flags);

    memcpy(out_msg, &tmp, sizeof(ipc_msg_t));
    return 0;
  }

  // Bloqueante.
  int rc = wait_event_interruptible(&mb->wq, ipc_mailbox_has_message, mb);
  if (rc < 0)
    return rc;

  // La condición era cierta. Pop con lock. Si otro consumer se llevó el
  // mensaje entre el wake_up y el lock, count será 0 y volvemos a dormir.
  while (1) {
    unsigned long flags = mailbox_lock(mb);
    if (mb->count > 0) {
      uint8_t head = mb->head;
      ipc_msg_t tmp;
      memcpy(&tmp, &mb->messages[head], sizeof(ipc_msg_t));
      mb->head = (head + 1) % IPC_MAILBOX_SIZE;
      mb->count--;
      mailbox_unlock(mb, flags);

      memcpy(out_msg, &tmp, sizeof(ipc_msg_t));
      return 0;
    }
    mailbox_unlock(mb, flags);

    rc = wait_event_interruptible(&mb->wq, ipc_mailbox_has_message, mb);
    if (rc < 0)
      return rc;
  }
}

int ipc_has_message(task_t *task) {
  if (!task)
    return 0;
  ipc_mailbox_t *mb = __atomic_load_n(&task->mailbox, __ATOMIC_ACQUIRE);
  if (!mb)
    return 0;
  unsigned long flags = mailbox_lock(mb);
  int has = mb->count > 0;
  mailbox_unlock(mb, flags);
  return has;
}