// kernel/ipc.c
#include "ipc.h"
#include "cpu.h"
#include "klog.h"
#include "serial.h"
#include "string.h"
#include "wait.h"

void ipc_init(void) {
  LOG_INFO("[IPC] Subsistema de Paso de Mensajes inicializado.");
}

int ipc_send(uint32_t target_id, uint32_t type, const void *data, size_t size) {
  task_t *current = sched_current();
  if (!current)
    return -1;

  task_t *target = sched_find_task(target_id);
  if (!target)
    return -1;

  if (target->mailbox.count >= IPC_MAILBOX_SIZE)
    return -2;

  uint8_t tail = target->mailbox.tail;
  ipc_msg_t *msg = &target->mailbox.messages[tail];

  msg->sender = current->id;
  msg->receiver = target_id;
  msg->type = type;

  size_t copy_len = size > IPC_MAX_PAYLOAD ? IPC_MAX_PAYLOAD : size;
  msg->size = copy_len;
  if (data && copy_len > 0) {
    stac(); // 'data' puede ser de usuario
    memcpy(msg->data, data, copy_len);
    clac();
  }

  target->mailbox.tail = (tail + 1) % IPC_MAILBOX_SIZE;
  target->mailbox.count++;

  // Despertar a quien esté esperando en el buzón del target.
  wake_up_all(&target->mailbox.wq);

  return 0;
}

// Condición que evalúa wait_event para el receptor.
// Se llama con el lock de la wq cogido (ver wait.c), pero NO con el
// lock del mailbox. Leer count sin lock aquí es benigno: si es > 0
// hay mensaje; si es 0 pero el emisor está en ipc_send justo antes
// del wake_up, el wake_up que sigue hará que re-chequee.
static bool ipc_mailbox_has_message(void *arg) {
  task_t *t = (task_t *)arg;
  return t->mailbox.count > 0;
}

int ipc_recv(ipc_msg_t *out_msg, int non_blocking) {
  task_t *current = sched_current();
  if (!current || !out_msg)
    return -1;

  // Camino no bloqueante: intentar pop directo.
  if (non_blocking) {
    if (current->mailbox.count == 0)
      return -1;
  } else {
    // Camino bloqueante. wait_event_interruptible re-chequea la
    // condición bajo el lock de la wq tras cada wake_up, así que
    // no hay race con ipc_send.
    int rc = wait_event_interruptible(&current->mailbox.wq,
                                      ipc_mailbox_has_message, current);
    if (rc < 0)
      return rc;
    // Al volver, la condición era cierta en el momento del chequeo.
    // Si otro consumidor se llevó el mensaje, ipc_mailbox_has_message
    // volverá a ser false y wait_event volverá a dormir (por eso
    // no hay que re-chequear count aquí).
  }

  uint8_t head = current->mailbox.head;
  stac(); // 'out_msg' es de usuario
  memcpy(out_msg, &current->mailbox.messages[head], sizeof(ipc_msg_t));
  clac();

  current->mailbox.head = (head + 1) % IPC_MAILBOX_SIZE;
  current->mailbox.count--;

  return 0;
}

int ipc_has_message(task_t *task) {
  if (!task)
    return 0;
  return task->mailbox.count > 0;
}