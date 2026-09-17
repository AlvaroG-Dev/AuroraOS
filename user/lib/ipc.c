#include "ipc.h"
#include "string.h"

int ipc_send(uint32_t target_id, uint32_t type, const void *data, size_t size) {
  return sys_ipc_send(target_id, type, data, size);
}

int ipc_send_text(uint32_t target_id, const char *text) {
  if (!text) return -1;
  return sys_ipc_send(target_id, IPC_TYPE_TEXT, text, strlen(text) + 1);
}

int ipc_recv(ipc_msg_t *msg, int non_blocking) {
  return sys_ipc_recv(msg, non_blocking);
}

uint32_t ipc_get_id(void) {
  return sys_get_task_id();
}
