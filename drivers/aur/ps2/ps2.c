// drivers/aur/ps2/ps2.c
//
// [FASE 5.5] ps2.aur — driver del controlador i8042 (teclado + ratón PS/2)
// en Ring 3.
//
// Usa solo primitivas de usuario:
//   - Puertos : /dev/inputproxy (INPUTP_PORT_IN/OUT; el kernel solo deja
//               tocar 0x60 y 0x64)
//   - IRQ     : /dev/irq/1 (teclado) y /dev/irq/12 (ratón) con epoll
//   - Entrada : INPUTP_FEED entrega al kernel los bytes crudos; la
//               traducción scancode -> TTY/input sigue en el kernel.
//
// Solo actúa si existe /usr/lib/aur/ps2.user (ver `make PS2_USER=1`); sin
// él, el driver de kernel ps2.c sigue siendo el dueño del controlador.
// Sale con código 0 en ese caso para que el supervisor no lo relance.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

// ============================================================================
// ABI con el kernel (copia de kernel/aur.h: el toolchain de userland no ve
// los headers del kernel)
// ============================================================================
#define INPUTP_PORT_IN 0x600
#define INPUTP_PORT_OUT 0x601
#define INPUTP_FEED 0x602
#define INPUTP_MOUSE_WHEEL 0x603

#define AUR_INPUT_KBD 0
#define AUR_INPUT_MOUSE 1
#define AUR_INPUT_MAX_FEED 64

struct aur_input_port {
  uint16_t port;
  uint8_t value;
  uint8_t _pad;
};
struct aur_input_feed {
  uint32_t kind;
  uint32_t len;
  uint8_t data[AUR_INPUT_MAX_FEED];
};
struct aur_irq_event {
  uint32_t irq;
  uint32_t count;
};

_Static_assert(sizeof(struct aur_input_port) == 4, "ABI aur_input_port");
_Static_assert(sizeof(struct aur_input_feed) == 72, "ABI aur_input_feed");

#define MARKER_PATH "/usr/lib/aur/ps2.user"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64 // lectura: estado; escritura: comando
#define ST_OBF 0x01     // byte disponible en 0x60
#define ST_IBF 0x02     // el controlador aún no ha leído la entrada
#define ST_AUX 0x20     // el byte viene del ratón

static int g_io = -1;

static int port_in(uint16_t port) {
  struct aur_input_port p = {.port = port};
  if (ioctl(g_io, INPUTP_PORT_IN, &p) < 0)
    return -1;
  return p.value;
}

static int port_out(uint16_t port, uint8_t val) {
  struct aur_input_port p = {.port = port, .value = val};
  return ioctl(g_io, INPUTP_PORT_OUT, &p);
}

static int wait_ibf_clear(void) {
  for (int i = 0; i < 5000; i++) {
    int s = port_in(PS2_STATUS);
    if (s < 0)
      return 0;
    if (!(s & ST_IBF))
      return 1;
    usleep(20);
  }
  return 0;
}

static int wait_obf_set(void) {
  for (int i = 0; i < 5000; i++) {
    int s = port_in(PS2_STATUS);
    if (s < 0)
      return 0;
    if (s & ST_OBF)
      return 1;
    usleep(20);
  }
  return 0;
}

static void drain_output(void) {
  for (int i = 0; i < 64; i++) {
    int s = port_in(PS2_STATUS);
    if (s < 0 || !(s & ST_OBF))
      return;
    port_in(PS2_DATA);
  }
}

static int ctl_cmd(uint8_t cmd) {
  return wait_ibf_clear() ? port_out(PS2_STATUS, cmd) : -1;
}

static int ctl_data(uint8_t val) {
  return wait_ibf_clear() ? port_out(PS2_DATA, val) : -1;
}

// Lee un byte del ratón (-1 si no llega).
static int mouse_read_byte(void) {
  for (int i = 0; i < 2000; i++) {
    int s = port_in(PS2_STATUS);
    if (s >= 0 && (s & ST_OBF) && (s & ST_AUX))
      return port_in(PS2_DATA);
    usleep(20);
  }
  return -1;
}

// Misma secuencia que el driver de kernel: habilita el puerto auxiliar,
// activa IRQ de teclado y ratón, y manda "enable data reporting" (0xF4) al
// ratón.
static void i8042_init(void) {
  ctl_cmd(0xA8); // habilitar puerto auxiliar
  drain_output();

  ctl_cmd(0x20); // leer command byte
  if (wait_obf_set()) {
    int cfg = port_in(PS2_DATA);
    if (cfg >= 0) {
      cfg |= 0x03;  // IRQ1 + IRQ12
      cfg &= ~0x30; // relojes de ambos puertos activos
      ctl_cmd(0x60);
      ctl_data((uint8_t)cfg);
    }
  }

  // Secuencia magica del IntelliMouse (sample rate 200, 100, 80) y lectura del
  // ID: 3 = tiene rueda y los paquetes pasan a ser de 4 bytes.
  static const uint8_t magic[] = {0xF3, 200, 0xF3, 100, 0xF3, 80, 0xF2};
  int ok = 1;
  for (unsigned i = 0; i < sizeof(magic) && ok; i++) {
    ctl_cmd(0xD4);
    ctl_data(magic[i]);
    ok = mouse_read_byte() == 0xFA;
  }
  if (ok && mouse_read_byte() == 3) {
    ioctl(g_io, INPUTP_MOUSE_WHEEL, 1);
    fprintf(stderr, "ps2.aur: IntelliMouse detectado (rueda)\n");
  }

  int ack = 0;
  for (int retry = 0; retry < 3 && !ack; retry++) {
    ctl_cmd(0xD4); // siguiente byte -> ratón
    ctl_data(0xF4);
    for (int i = 0; i < 2000; i++) {
      int s = port_in(PS2_STATUS);
      if (s >= 0 && (s & ST_OBF) && (s & ST_AUX)) {
        if (port_in(PS2_DATA) == 0xFA) {
          ack = 1;
          break;
        }
      }
      usleep(20);
    }
  }
  fprintf(stderr, "ps2.aur: ratón %s\n", ack ? "habilitado (ACK 0xF4)" : "sin ACK");
}

// Vacía el controlador entregando al kernel lo que haya. Los bytes de teclado
// y de ratón se agrupan por tipo y se envían en lotes para ahorrar syscalls.
static int pump(void) {
  struct aur_input_feed f;
  memset(&f, 0, sizeof(f));
  int total = 0;

  for (int budget = 0; budget < 256; budget++) {
    int s = port_in(PS2_STATUS);
    if (s < 0)
      return -1;
    if (!(s & ST_OBF))
      break;
    int b = port_in(PS2_DATA);
    if (b < 0)
      return -1;
    uint32_t kind = (s & ST_AUX) ? AUR_INPUT_MOUSE : AUR_INPUT_KBD;

    if (f.len && (f.kind != kind || f.len == AUR_INPUT_MAX_FEED)) {
      if (ioctl(g_io, INPUTP_FEED, &f) < 0)
        return -1;
      f.len = 0;
    }
    if (f.len == 0)
      f.kind = kind;
    f.data[f.len++] = (uint8_t)b;
    total++;
  }
  if (f.len) {
    if (ioctl(g_io, INPUTP_FEED, &f) < 0)
      return -1;
  }
  return total;
}

static int open_irq(int n) {
  char path[32];
  snprintf(path, sizeof(path), "/dev/irq/%d", n);
  return open(path, O_RDONLY);
}

int main(void) {
  if (access(MARKER_PATH, F_OK) != 0) {
    // Modo kernel: nada que hacer. Código 0 = salida voluntaria.
    return 0;
  }

  g_io = open("/dev/inputproxy", O_RDWR);
  if (g_io < 0) {
    perror("ps2.aur: /dev/inputproxy");
    return 1;
  }

  int irq_kbd = open_irq(1);
  int irq_mouse = open_irq(12);
  if (irq_kbd < 0 || irq_mouse < 0) {
    perror("ps2.aur: /dev/irq");
    return 1;
  }

  i8042_init();
  drain_output();

  int ep = epoll_create1(0);
  if (ep < 0) {
    perror("ps2.aur: epoll_create1");
    return 1;
  }
  struct epoll_event ev = {.events = EPOLLIN};
  ev.data.fd = irq_kbd;
  epoll_ctl(ep, EPOLL_CTL_ADD, irq_kbd, &ev);
  ev.data.fd = irq_mouse;
  epoll_ctl(ep, EPOLL_CTL_ADD, irq_mouse, &ev);

  fprintf(stderr, "ps2.aur: listo (IRQ 1 y 12)\n");

  for (;;) {
    struct epoll_event out[2];
    // Timeout corto: si una IRQ se pierde (edge) el siguiente latido recoge
    // lo que haya en el controlador.
    int n = epoll_wait(ep, out, 2, 100);
    if (n < 0 && errno != EINTR) {
      perror("ps2.aur: epoll_wait");
      return 1;
    }
    for (int i = 0; i < n; i++) {
      struct aur_irq_event e;
      if (read(out[i].data.fd, &e, sizeof(e)) < 0 && errno != EINTR)
        perror("ps2.aur: read irq");
    }
    if (pump() < 0) {
      perror("ps2.aur: pump");
      return 1;
    }
  }
}
