// aurora-vm — VMM de la mini-VM de AuroraOS (Plan_MiniVM.md).
//
// F1: ejecuta un binario plano en modo real sobre /dev/aurora-vm. El puerto
// 0x3F8 escribe en la salida estandar y 0xF4 termina con el codigo escrito.
// Los .exe necesitan un huesped completo (F3 en adelante): por ahora se avisa.
//
// Uso:
//   aurora-vm --caps                 que ofrece la CPU (VMX, EPT, ...)
//   aurora-vm --autotest             guest incorporado: debe imprimir "Hola desde la VM"
//   aurora-vm --plano FICH [--en DIR] [--mem MiB]
//   aurora-vm PROGRAMA.exe           (aurora-run lo llama asi; aun sin huesped)
#define _GNU_SOURCE
#include "../../../../kernel/aurora_vm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEV "/dev/aurora-vm"

// hola.asm de tests/vm, ensamblado a mano (16 bits, ORG 0x1000)
static const uint8_t guest_hola[] = {
    0xBE, 0x0F, 0x10, 0xAC, 0x84, 0xC0, 0x74, 0x06, 0xBA, 0xF8, 0x03,
    0xEE, 0xEB, 0xF5, 0xF4,
    'H', 'o', 'l', 'a', ' ', 'd', 'e', 's', 'd', 'e', ' ', 'l', 'a', ' ', 'V',
    'M', '\n', 0};

static int open_vm(void) {
  int fd = open(DEV, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "aurora-vm: no puedo abrir %s: %s\n", DEV, strerror(errno));
    fprintf(stderr, "  (sin VT-x/EPT en esta CPU, o virtualizacion apagada en la BIOS)\n");
  }
  return fd;
}

static int cmd_caps(void) {
  int fd = open_vm();
  if (fd < 0)
    return 1;
  struct avm_caps c;
  if (ioctl(fd, AVM_GET_CAPS, &c) < 0) {
    perror("AVM_GET_CAPS");
    return 1;
  }
  printf("VMX: %s\nEPT: %s\nunrestricted guest: %s\nVPID: %s\nusable: %s\n"
         "revision VMCS: %u\nEPT/VPID cap: 0x%llx\n",
         c.flags & AVM_CAP_VMX ? "si" : "no", c.flags & AVM_CAP_EPT ? "si" : "no",
         c.flags & AVM_CAP_UNRESTRICTED ? "si" : "no",
         c.flags & AVM_CAP_VPID ? "si" : "no", c.flags & AVM_CAP_USABLE ? "SI" : "no",
         c.vmcs_revision, (unsigned long long)c.ept_vpid_cap);
  return (c.flags & AVM_CAP_USABLE) ? 0 : 1;
}

static int run_flat(const uint8_t *code, size_t len, uint64_t at, unsigned mem_mb,
                    int expect_text) {
  int vm = open_vm();
  if (vm < 0)
    return 1;
  uint64_t size = (uint64_t)mem_mb << 20;
  int mfd = memfd_create("aurora-vm-ram", MFD_CLOEXEC);
  if (mfd < 0 || ftruncate(mfd, (off_t)size) < 0) {
    perror("memfd");
    return 1;
  }
  uint8_t *ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
  if (ram == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  if (at + len > size) {
    fprintf(stderr, "aurora-vm: el binario no cabe en la RAM del huesped\n");
    return 1;
  }
  memcpy(ram + at, code, len);
  struct avm_memory m = {.fd = mfd, .slot = 0, .gpa = 0, .size = size, .offset = 0};
  if (ioctl(vm, AVM_SET_MEMORY, &m) < 0) {
    perror("AVM_SET_MEMORY");
    return 1;
  }
  struct avm_regs r;
  if (ioctl(vm, AVM_GET_REGS, &r) < 0) {
    perror("AVM_GET_REGS");
    return 1;
  }
  // Modo real plano: todos los segmentos en 0, pila en el primer MiB.
  struct avm_seg cs = {.base = 0, .limit = 0xFFFF, .selector = 0, .ar = 0x9B};
  struct avm_seg ds = {.base = 0, .limit = 0xFFFF, .selector = 0, .ar = 0x93};
  r.cs = cs;
  r.ds = r.es = r.fs = r.gs = r.ss = ds;
  r.rip = at;
  r.rsp = 0x8000;
  r.rflags = 2;
  if (ioctl(vm, AVM_SET_REGS, &r) < 0) {
    perror("AVM_SET_REGS");
    return 1;
  }
  struct avm_exit x;
  memset(&x, 0, sizeof(x));
  for (unsigned n = 0; n < 10000000; n++) {
    if (ioctl(vm, AVM_RUN, &x) < 0) {
      perror("AVM_RUN");
      return 1;
    }
    switch (x.reason) {
    case AVM_EXIT_IO:
      if (x.io.is_write && x.io.port == 0x3F8) {
        putchar((int)(x.io.data & 0xFF));
        fflush(stdout);
      } else if (x.io.is_write && x.io.port == 0xF4) {
        return (int)(x.io.data & 0xFF);
      } else if (!x.io.is_write) {
        x.io.data = 0xFFFFFFFFu; // puerto sin dispositivo
      }
      break;
    case AVM_EXIT_HLT:
      (void)expect_text;
      return 0;
    case AVM_EXIT_SIGNAL:
      break;
    case AVM_EXIT_SHUTDOWN:
      fprintf(stderr, "aurora-vm: el huesped se apago (triple fallo)\n");
      return 2;
    case AVM_EXIT_MMIO:
      fprintf(stderr, "aurora-vm: acceso MMIO en 0x%llx (sin dispositivo; F2)\n",
              (unsigned long long)x.mmio.gpa);
      return 2;
    default:
      fprintf(stderr,
              "aurora-vm: fallo de VMX (salida %u, motivo %u, codigo %llu, qual 0x%llx); "
              "mira dmesg: lineas [VMX]\n",
              x.reason, x.hw_reason, (unsigned long long)x.fail.code,
              (unsigned long long)x.fail.qual);
      return 3;
    }
  }
  fprintf(stderr, "aurora-vm: demasiadas salidas sin terminar\n");
  return 4;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Uso: aurora-vm --caps | --autotest | --plano FICH [--en DIR] [--mem MiB] | PROGRAMA.exe\n");
    return 2;
  }
  if (!strcmp(argv[1], "--caps"))
    return cmd_caps();
  if (!strcmp(argv[1], "--autotest"))
    return run_flat(guest_hola, sizeof(guest_hola), 0x1000, 2, 1);
  if (!strcmp(argv[1], "--plano") && argc >= 3) {
    uint64_t at = 0x1000;
    unsigned mem = 16;
    for (int i = 3; i + 1 < argc; i += 2) {
      if (!strcmp(argv[i], "--en"))
        at = strtoull(argv[i + 1], NULL, 0);
      else if (!strcmp(argv[i], "--mem"))
        mem = (unsigned)strtoul(argv[i + 1], NULL, 0);
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) {
      perror(argv[2]);
      return 1;
    }
    uint8_t *buf = malloc(1 << 20);
    size_t n = buf ? fread(buf, 1, 1 << 20, f) : 0;
    fclose(f);
    if (!n) {
      fprintf(stderr, "aurora-vm: %s vacio o ilegible\n", argv[2]);
      return 1;
    }
    return run_flat(buf, n, at, mem, 0);
  }
  fprintf(stderr,
          "aurora-vm: %s: ejecutar programas Windows necesita un huesped completo "
          "(ReactOS/Windows en una imagen), que llega en las fases F3-F5 de "
          "Plan_MiniVM.md. Por ahora solo --caps, --autotest y --plano.\n",
          argv[1]);
  return 126;
}
