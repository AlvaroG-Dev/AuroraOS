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

void ipc_init(void) {
  LOG_INFO("[IPC] Subsistema de Paso de Mensajes inicializado.");
}

// ---------------------------------------------------------------------------
// Helpers de lock del mailbox.
// ---------------------------------------------------------------------------
static unsigned long mailbox_lock(task_t *t) {
  return spin_lock_irqsave(&t->mailbox.wq.lock);
}

static void mailbox_unlock(task_t *t, unsigned long flags) {
  spin_unlock_irqrestore(&t->mailbox.wq.lock, flags);
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

  task_t *target = sched_find_task(target_id);
  if (!target)
    return -1;

  // Copiar payload a un buffer local ANTES de coger el lock.
  // `data` ya es del kernel, así que memcpy directo es seguro.
  uint8_t tmp[IPC_MAX_PAYLOAD];
  size_t copy_len = size > IPC_MAX_PAYLOAD ? IPC_MAX_PAYLOAD : size;
  if (data && copy_len > 0) {
    memcpy(tmp, data, copy_len);
  }

  unsigned long flags = mailbox_lock(target);

  if (target->mailbox.count >= IPC_MAILBOX_SIZE) {
    mailbox_unlock(target, flags);
    return -2;
  }

  uint8_t tail = target->mailbox.tail;
  ipc_msg_t *msg = &target->mailbox.messages[tail];

  msg->sender = current->id;
  msg->receiver = target_id;
  msg->type = type;
  msg->size = (uint32_t)copy_len;
  if (copy_len > 0) {
    memcpy(msg->data, tmp, copy_len);
  }

  target->mailbox.tail = (tail + 1) % IPC_MAILBOX_SIZE;
  target->mailbox.count++;

  // Atómico con count++: el lock ya está cogido.
  wake_up_all_locked(&target->mailbox.wq);

  mailbox_unlock(target, flags);
  return 0;
}

// ---------------------------------------------------------------------------
// Condición de la wait queue del mailbox.
// Se llama con el lock del mailbox (= lock de la wq) cogido.
// ---------------------------------------------------------------------------
static bool ipc_mailbox_has_message(void *arg) {
  task_t *t = (task_t *)arg;
  return t->mailbox.count > 0;
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

  if (non_blocking) {
    unsigned long flags = mailbox_lock(current);
    if (current->mailbox.count == 0) {
      mailbox_unlock(current, flags);
      return -1;
    }
    uint8_t head = current->mailbox.head;
    // Copiamos a un buffer local para no exponer el mailbox
    // mientras tenemos el lock. Es del kernel, memcpy directo.
    ipc_msg_t tmp;
    memcpy(&tmp, &current->mailbox.messages[head], sizeof(ipc_msg_t));
    current->mailbox.head = (head + 1) % IPC_MAILBOX_SIZE;
    current->mailbox.count--;
    mailbox_unlock(current, flags);

    memcpy(out_msg, &tmp, sizeof(ipc_msg_t));
    return 0;
  }

  // Bloqueante.
  int rc = wait_event_interruptible(&current->mailbox.wq,
                                    ipc_mailbox_has_message, current);
  if (rc < 0)
    return rc;

  // La condición era cierta. Pop con lock. Si otro consumer se llevó el
  // mensaje entre el wake_up y el lock, count será 0 y volvemos a dormir.
  while (1) {
    unsigned long flags = mailbox_lock(current);
    if (current->mailbox.count > 0) {
      uint8_t head = current->mailbox.head;
      ipc_msg_t tmp;
      memcpy(&tmp, &current->mailbox.messages[head], sizeof(ipc_msg_t));
      current->mailbox.head = (head + 1) % IPC_MAILBOX_SIZE;
      current->mailbox.count--;
      mailbox_unlock(current, flags);

      memcpy(out_msg, &tmp, sizeof(ipc_msg_t));
      return 0;
    }
    mailbox_unlock(current, flags);

    rc = wait_event_interruptible(&current->mailbox.wq, ipc_mailbox_has_message,
                                  current);
    if (rc < 0)
      return rc;
  }
}

int ipc_has_message(task_t *task) {
  if (!task)
    return 0;
  unsigned long flags = mailbox_lock(task);
  int has = task->mailbox.count > 0;
  mailbox_unlock(task, flags);
  return has;
}