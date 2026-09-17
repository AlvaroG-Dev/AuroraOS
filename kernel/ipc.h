// kernel/ipc.h
#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include "sched.h"
#include <stddef.h>
#include <stdint.h>

#define IPC_TYPE_RAW 0
#define IPC_TYPE_TEXT 1
#define IPC_TYPE_EVENT 2
#define IPC_TYPE_RESPONSE 3

void ipc_init(void);

// Envía un mensaje. Retorna 0 si OK, -1 si no existe el target,
// -2 si el buzón está lleno.
int ipc_send(uint32_t target_id, uint32_t type, const void *data, size_t size);

// Recibe un mensaje. Si non_blocking != 0 y no hay mensaje,
// retorna -1 inmediatamente. Si non_blocking == 0, duerme en la
// wait queue del buzón hasta que llegue un mensaje.
int ipc_recv(ipc_msg_t *out_msg, int non_blocking);

int ipc_has_message(task_t *task);

#endif