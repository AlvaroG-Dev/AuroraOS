// test_memfd.c — smoke test de memfd_create(2).
//
// musl no expone MFD_* en sys/mman.h: son de <linux/memfd.h>. Los
// definimos aquí para no depender de headers de kernel.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

// F_ADD_SEALS / F_GET_SEALS (linux/fcntl.h). musl <fcntl.h> no las trae.
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif

#ifndef F_SEAL_WRITE
#define F_SEAL_WRITE 0x0008U
#endif

#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif

int main(void) {
  int fd =
      (int)syscall(SYS_memfd_create, "test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    perror("memfd_create");
    return 1;
  }
  printf("memfd fd=%d\n", fd);

  const char *msg = "hola memfd";
  ssize_t w = write(fd, msg, 10);
  if (w != 10) {
    perror("write");
    return 1;
  }

  if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) < 0) {
    perror("F_ADD_SEALS");
    return 1;
  }

  int seals = fcntl(fd, F_GET_SEALS);
  if (seals < 0) {
    perror("F_GET_SEALS");
    return 1;
  }
  printf("seals=0x%x\n", seals);

  // Tras F_SEAL_WRITE, un write debe fallar con EPERM.
  ssize_t w2 = write(fd, "X", 1);
  if (w2 >= 0) {
    fprintf(stderr, "write tras seal: esperaba EPERM, devolvió %zd\n", w2);
    return 1;
  }
  perror("write tras seal"); // debe imprimir "Operation not permitted"

  if (lseek(fd, 0, SEEK_SET) < 0) {
    perror("lseek");
    return 1;
  }
  char buf[16] = {0};
  ssize_t r = read(fd, buf, 10);
  if (r != 10) {
    perror("read");
    return 1;
  }
  printf("read: %s\n", buf);

  close(fd);
  printf("OK\n");
  return 0;
}