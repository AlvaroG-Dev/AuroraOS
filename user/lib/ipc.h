#ifndef USER_IPC_H
#define USER_IPC_H

#include "../syscall.h"
#include <stdint.h>
#include <stddef.h>

int ipc_send(uint32_t target_id, uint32_t type, const void *data, size_t size);
int ipc_send_text(uint32_t target_id, const char *text);
int ipc_recv(ipc_msg_t *msg, int non_blocking);
uint32_t ipc_get_id(void);

#endif
