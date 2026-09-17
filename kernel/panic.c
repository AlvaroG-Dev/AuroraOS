// kernel/panic.c
// Backtrace simple siguiendo la cadena de RBP.

#include "panic.h"
#include "serial.h"

extern uint8_t __text_start;
extern uint8_t __text_end;

static int is_kernel_text(uint64_t addr) {
  uint64_t start = (uint64_t)&__text_start;
  uint64_t end = (uint64_t)&__text_end;
  return addr >= start && addr < end;
}

void backtrace(uint64_t rbp, uint64_t rip, int max_frames) {
  serial_puts("\n[BT] Stack trace:");
  serial_puts("\n[BT]   #0 ");
  serial_hex(rip);

  uint64_t frame_rbp = rbp;
  int frame = 1;

  while (frame < max_frames) {
    if (frame_rbp == 0 || (frame_rbp & 0x7) != 0)
      break;

    uint64_t *f = (uint64_t *)frame_rbp;
    uint64_t next_rbp = f[0];
    uint64_t ret_rip = f[1];

    if (!is_kernel_text(ret_rip))
      break;

    serial_puts("\n[BT]   #");
    serial_putn(frame, 10, 0);
    serial_puts(" ");
    serial_hex(ret_rip);

    frame_rbp = next_rbp;
    frame++;
  }

  if (frame >= max_frames) {
    serial_puts("\n[BT]   ... (más de ");
    serial_putn(max_frames, 10, 0);
    serial_puts(" frames)");
  }
  serial_puts("\n[BT] Fin del backtrace.\n");
}