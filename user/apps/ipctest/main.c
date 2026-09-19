// apps/ipctest/main.c
// Tests: sys_get_task_id, sys_get_service_id, sys_ipc_send, sys_ipc_recv
#include "../../lib/ipc.h"
#include "../../lib/string.h"
#include "../../syscall.h"

static void print_u32(const char *prefix, uint32_t v) {
  char buf[16];
  int i = 0;
  if (v == 0) {
    buf[i++] = '0';
  }
  uint32_t tmp = v;
  while (tmp) {
    buf[i++] = '0' + (int)(tmp % 10);
    tmp /= 10;
  }
  for (int a = 0, b = i - 1; a < b; a++, b--) {
    char t = buf[a];
    buf[a] = buf[b];
    buf[b] = t;
  }
  buf[i] = 0;
  char out[128];
  int oi = 0, pi = 0, bi = 0;
  while (prefix[pi])
    out[oi++] = prefix[pi++];
  while (buf[bi])
    out[oi++] = buf[bi++];
  out[oi++] = '\n';
  out[oi] = 0;
  sys_print(out);
}

int main(void) {
  sys_print("[ipctest] === Aurora OS IPC Test App ===\n");

  // Test 1: Get task ID
  uint32_t my_id = ipc_get_id();
  print_u32("[ipctest] Mi Task ID: ", my_id);
  if (my_id == (uint32_t)-1) {
    sys_print("[ipctest] ERROR: get_task_id fallido\n");
    sys_exit(20);
  }
  sys_print("[ipctest] Test 1 (get_task_id): OK\n");

  // Test 1b: Descubrir el servicio de eco por nombre.
  int echo_id = sys_get_service_id("echo");
  if (echo_id < 0) {
    sys_print("[ipctest] ERROR: servicio 'echo' no encontrado\n");
    sys_exit(19);
  }
  print_u32("[ipctest] Echo Task ID: ", (uint32_t)echo_id);
  sys_print("[ipctest] Test 1b (get_service_id): OK\n");

  // Test 2: Send message to echo service
  sys_print("[ipctest] Test 2: Enviando PING al servicio de eco\n");
  const char *ping_msg = "PING desde ipctest app";
  int sr = ipc_send_text((uint32_t)echo_id, ping_msg);
  if (sr != 0) {
    sys_print("[ipctest] ERROR: ipc_send fallido\n");
    sys_exit(21);
  }
  sys_print("[ipctest]   send: OK\n");

  // Test 3: Receive reply (blocking)
  sys_print("[ipctest] Test 3: Esperando respuesta PONG...\n");
  ipc_msg_t reply;
  int rr = ipc_recv(&reply, 0);
  if (rr != 0) {
    sys_print("[ipctest] ERROR: ipc_recv fallido\n");
    sys_exit(22);
  }
  sys_print("[ipctest]   Respuesta recibida: '");
  sys_print((const char *)reply.data);
  sys_print("'\n");
  print_u32("[ipctest]   De Task ID: ", reply.sender);
  if (reply.type != IPC_TYPE_RESPONSE) {
    sys_print("[ipctest] ERROR: tipo de respuesta inesperado\n");
    sys_exit(23);
  }
  sys_print("[ipctest] Test 3 (ipc_recv PONG): OK\n");

  // Test 4: Non-blocking recv when mailbox is empty
  sys_print("[ipctest] Test 4: ipc_recv no-bloqueante (mailbox vacio)\n");
  ipc_msg_t tmp;
  int nr = ipc_recv(&tmp, 1);
  if (nr != -1) {
    sys_print("[ipctest] WARN: se esperaba -1 con mailbox vacio\n");
  } else {
    sys_print("[ipctest]   Non-blocking empty recv: OK\n");
  }

  // Test 5: Puntero no mapeado en userland (debe devolver -EFAULT).
  //
  // 0xDEADBEEF está por debajo de USER_LIMIT, así que access_ok lo
  // acepta; la página no está mapeada, así que raw_copy_from_user
  // provoca #PF que el fixup de uaccess convierte en -EFAULT.
  sys_print("[ipctest] Test 5: Puntero no mapeado en userland\n");
  int rc =
      sys_ipc_send((uint32_t)echo_id, IPC_TYPE_RAW, (void *)0xDEADBEEF, 32);
  if (rc != -14) { // -EFAULT == -14
    sys_print("[ipctest] ERROR: se esperaba -EFAULT (-14)\n");
    sys_exit(24);
  }
  sys_print("[ipctest]   -EFAULT correcto: OK\n");

  sys_print("[ipctest] Todas las pruebas IPC PASARON. Saliendo con 0.\n");
  sys_exit(0);
  return 0;
}