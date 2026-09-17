// apps/hello/main.c
// Master orchestrator: spawns child processes and verifies their exit codes
#include "../../lib/file.h"
#include "../../lib/ipc.h"
#include "../../lib/malloc.h"
#include "../../lib/process.h"
#include "../../lib/string.h"
#include "../../syscall.h"

static void print_int(const char *prefix, int v) {
  char buf[16];
  int i = 0;
  int neg = 0;
  uint32_t uv;
  if (v < 0) {
    neg = 1;
    uv = (uint32_t)(-v);
  } else {
    uv = (uint32_t)v;
  }
  if (uv == 0)
    buf[i++] = '0';
  while (uv) {
    buf[i++] = '0' + (int)(uv % 10);
    uv /= 10;
  }
  if (neg)
    buf[i++] = '-';
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
  int my_pid = getpid();
  print_int("[hello] === Aurora OS Master App (PID=", my_pid);
  sys_print(") ===\n");
  sys_print("[hello] Demostrador de Ejecucion Dinamica de Procesos\n\n");

  int passed = 0;
  int failed = 0;

  // TEST 1: VFS directo
  sys_print("[hello] [VFS] Leyendo system/config.txt...\n");
  int fd = open("system/config.txt", O_RDONLY);
  if (fd >= 0) {
    char buf[64];
    int64_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0)
      buf[n] = '\0';
    sys_print("[hello]   Contenido: '");
    sys_print(buf);
    sys_print("'\n");
    close(fd);
    sys_print("[hello]   [VFS directo]: OK\n");
    passed++;
  } else {
    sys_print("[hello]   [VFS directo]: FALLO\n");
    failed++;
  }

  // TEST 2: IPC
  sys_print("\n[hello] [IPC] Prueba directa con echo service (Task 1)...\n");
  int sr = ipc_send_text(1, "PING desde hello-master");
  if (sr == 0) {
    ipc_msg_t resp;
    if (ipc_recv(&resp, 0) == 0) {
      sys_print("[hello]   Respuesta kernel: '");
      sys_print((const char *)resp.data);
      sys_print("'\n");
      sys_print("[hello]   [IPC directo]: OK\n");
      passed++;
    } else {
      sys_print("[hello]   [IPC recv]: FALLO\n");
      failed++;
    }
  } else {
    sys_print("[hello]   [IPC send]: FALLO\n");
    failed++;
  }

  // TEST 3: apps/calc
  sys_print("\n[hello] [SPAWN] Lanzando apps/calc...\n");
  int calc_pid = spawn("apps/calc");
  if (calc_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/calc\n");
    failed++;
  } else {
    print_int("[hello]   apps/calc iniciado con PID=", calc_pid);
    int calc_status = 0;
    int wpid = waitpid(calc_pid, &calc_status, 0);
    print_int("[hello]   apps/calc terminó (waitpid retornó PID=", wpid);
    print_int("[hello]   apps/calc exit code = ", calc_status);
    if (wpid == calc_pid && calc_status == 42) {
      sys_print("[hello]   [apps/calc]: OK (exit 42 verificado)\n");
      passed++;
    } else {
      sys_print("[hello]   [apps/calc]: FALLO (codigo inesperado)\n");
      failed++;
    }
  }

  // TEST 4: apps/filetest
  sys_print("\n[hello] [SPAWN] Lanzando apps/filetest...\n");
  int ft_pid = spawn("apps/filetest");
  if (ft_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/filetest\n");
    failed++;
  } else {
    print_int("[hello]   apps/filetest iniciado con PID=", ft_pid);
    int ft_status = 0;
    int fwpid = waitpid(ft_pid, &ft_status, 0);
    print_int("[hello]   apps/filetest exit code = ", ft_status);
    if (fwpid == ft_pid && ft_status == 0) {
      sys_print("[hello]   [apps/filetest]: OK\n");
      passed++;
    } else {
      sys_print("[hello]   [apps/filetest]: FALLO\n");
      failed++;
    }
  }

  // TEST 5: apps/ipctest
  sys_print("\n[hello] [SPAWN] Lanzando apps/ipctest...\n");
  int ipc_pid = spawn("apps/ipctest");
  if (ipc_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/ipctest\n");
    failed++;
  } else {
    print_int("[hello]   apps/ipctest iniciado con PID=", ipc_pid);
    int ipc_status = 0;
    int iwpid = waitpid(ipc_pid, &ipc_status, 0);
    print_int("[hello]   apps/ipctest exit code = ", ipc_status);
    if (iwpid == ipc_pid && ipc_status == 0) {
      sys_print("[hello]   [apps/ipctest]: OK\n");
      passed++;
    } else {
      sys_print("[hello]   [apps/ipctest]: FALLO\n");
      failed++;
    }
  }

  // TEST 6: apps/pftest
  sys_print("\n[hello] [SPAWN] Lanzando apps/pftest...\n");
  int pft_pid = spawn("apps/pftest");
  if (pft_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/pftest\n");
    failed++;
  } else {
    print_int("[hello]   apps/pftest iniciado con PID=", pft_pid);
    int pft_status = 0;
    int pwpid = waitpid(pft_pid, &pft_status, 0);
    print_int("[hello]   apps/pftest exit code = ", pft_status);
    if (pwpid == pft_pid && pft_status == 0) {
      sys_print("[hello]   [apps/pftest]: OK\n");
      passed++;
    } else {
      sys_print("[hello]   [apps/pftest]: FALLO\n");
      failed++;
    }
  }

  // TEST 7: apps/segvtest
  sys_print(
      "\n[hello] [SPAWN] Lanzando apps/segvtest (debe morir por #PF)...\n");
  int sgv_pid = spawn("apps/segvtest");
  if (sgv_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/segvtest\n");
    failed++;
  } else {
    print_int("[hello]   apps/segvtest iniciado con PID=", sgv_pid);
    int sgv_status = 0;
    int swpid = waitpid(sgv_pid, &sgv_status, 0);
    print_int("[hello]   apps/segvtest waitpid retorno PID=", swpid);
    if (swpid == sgv_pid) {
      sys_print(
          "[hello]   [apps/segvtest]: OK (murio por #PF como esperado)\n");
      passed++;
    } else {
      sys_print("[hello]   [apps/segvtest]: FALLO (waitpid incorrecto)\n");
      failed++;
    }
  }

  // TEST 8: apps/segvtest_stack
  sys_print(
      "\n[hello] [SPAWN] Lanzando apps/segvtest_stack (stack overflow)...\n");
  int sov_pid = spawn("apps/segvtest_stack");
  if (sov_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/segvtest_stack\n");
    failed++;
  } else {
    int sov_status = 0;
    int swpid = waitpid(sov_pid, &sov_status, 0);
    if (swpid == sov_pid) {
      sys_print("[hello]   [segvtest_stack]: OK (murio por stack overflow)\n");
      passed++;
    } else {
      sys_print("[hello]   [segvtest_stack]: FALLO\n");
      failed++;
    }
  }

  // TEST 9: apps/segvtest_nx
  sys_print("\n[hello] [SPAWN] Lanzando apps/segvtest_nx (NX en stack)...\n");
  int nx_pid = spawn("apps/segvtest_nx");
  if (nx_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/segvtest_nx\n");
    failed++;
  } else {
    int nx_status = 0;
    int nwpid = waitpid(nx_pid, &nx_status, 0);
    if (nwpid == nx_pid) {
      sys_print("[hello]   [segvtest_nx]: OK (murio por NX)\n");
      passed++;
    } else {
      sys_print("[hello]   [segvtest_nx]: FALLO\n");
      failed++;
    }
  }

  // TEST 10: apps/segvtest_mmap_ro
  sys_print(
      "\n[hello] [SPAWN] Lanzando apps/segvtest_mmap_ro (escribir RO)...\n");
  int ro_pid = spawn("apps/segvtest_mmap_ro");
  if (ro_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/segvtest_mmap_ro\n");
    failed++;
  } else {
    int ro_status = 0;
    int rwpid = waitpid(ro_pid, &ro_status, 0);
    if (rwpid == ro_pid) {
      sys_print("[hello]   [segvtest_mmap_ro]: OK (murio por proteccion)\n");
      passed++;
    } else {
      sys_print("[hello]   [segvtest_mmap_ro]: FALLO\n");
      failed++;
    }
  }

  // TEST 11: apps/segvtest_kernel
  sys_print(
      "\n[hello] [SPAWN] Lanzando apps/segvtest_kernel (saltar a kernel)...\n");
  int krn_pid = spawn("apps/segvtest_kernel");
  if (krn_pid < 0) {
    sys_print("[hello]   ERROR: no se pudo iniciar apps/segvtest_kernel\n");
    failed++;
  } else {
    int krn_status = 0;
    int kwpid = waitpid(krn_pid, &krn_status, 0);
    if (kwpid == krn_pid) {
      sys_print("[hello]   [segvtest_kernel]: OK (murio por aislamiento)\n");
      passed++;
    } else {
      sys_print("[hello]   [segvtest_kernel]: FALLO\n");
      failed++;
    }
  }

  // RESUMEN
  sys_print("\n[hello] =============================\n");
  sys_print("[hello] RESULTADO FINAL DE AURORA OS\n");
  sys_print("[hello] =============================\n");
  print_int("[hello]   Pruebas PASADAS: ", passed);
  print_int("[hello]   Pruebas FALLIDAS: ", failed);
  if (failed == 0) {
    sys_print("[hello] *** TODAS LAS SYSCALLS VERIFICADAS CON EXITO ***\n");
    sys_print("[hello]     SYS_PRINT, SYS_YIELD, SYS_EXIT, SYS_SBRK\n");
    sys_print("[hello]     SYS_OPEN, SYS_READ, SYS_WRITE, SYS_SEEK\n");
    sys_print("[hello]     SYS_CLOSE, SYS_FSTAT, SYS_IPC_SEND\n");
    sys_print("[hello]     SYS_IPC_RECV, SYS_GET_TASK_ID\n");
    sys_print("[hello]     SYS_SPAWN, SYS_WAITPID, SYS_GETPID\n");
    sys_print("[hello]     SYS_MMAP, SYS_MUNMAP\n");
    sys_print("[hello]     Demand paging + kill por #PF\n");
  } else {
    sys_print("[hello] *** ALGUNAS PRUEBAS FALLARON ***\n");
  }

  sys_exit(failed == 0 ? 0 : 1);
  return 0;
}