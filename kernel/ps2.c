// kernel/ps2.c
#include "ps2.h"
#include "serial.h"
#include "idt.h"
#include "sched.h"
#include "gfx/compositor.h"
#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "dN"(port));
}

static inline void io_wait(void) {
    outb(0x80, 0x00);
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile ("push %0\n\tpopfq" :: "r"(flags) : "memory");
}

static int wait_input_buffer_empty(void) {
    for (int i = 0; i < 50000; i++) {
        if (!(inb(0x64) & 0x02)) return 1;
        io_wait();
    }
    return 0;
}

static int wait_output_buffer_full(void) {
    for (int i = 0; i < 50000; i++) {
        if (inb(0x64) & 0x01) return 1;
        io_wait();
    }
    return 0;
}

static void drain_output(void) {
    int i = 0;
    while ((inb(0x64) & 0x01) && i++ < 64) {
        (void)inb(0x60);
        io_wait();
    }
}

#define KBD_BUF_SIZE 256
#define MOUSE_BUF_SIZE 128

static volatile uint8_t kbuf[KBD_BUF_SIZE];
static volatile uint8_t khead = 0, ktail = 0;

typedef struct { 
    int16_t dx; 
    int16_t dy; 
    uint8_t buttons; 
} mouse_evt_t;

static volatile mouse_evt_t mbuf[MOUSE_BUF_SIZE];
static volatile uint8_t mhead = 0, mtail = 0;

static volatile int mouse_cycle = 0;
static uint8_t mouse_packet[3];

static void ps2_keyboard_irq(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01) || (status & 0x20)) return;

    uint8_t sc = inb(0x60);
    uint8_t next = (khead + 1) & (KBD_BUF_SIZE - 1);
    if (next != ktail) {
        kbuf[khead] = sc;
        khead = next;
    }
}

static void ps2_mouse_irq(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01) || !(status & 0x20)) return;

    uint8_t data = inb(0x60);

    switch (mouse_cycle) {
        case 0:
            // Bit 3 debe ser 1 en el primer byte de cabecera PS/2
            if (data & 0x08) {
                mouse_packet[0] = data;
                mouse_cycle = 1;
            }
            break;

        case 1:
            mouse_packet[1] = data;
            mouse_cycle = 2;
            break;

        case 2:
            mouse_packet[2] = data;
            mouse_cycle = 0;

            // Descartar si se declara desbordamiento de hardware (bits 6 u 7)
            if (mouse_packet[0] & 0xC0) break;

            // Extensión de signo de 9 bits precisa
            int16_t raw_x = mouse_packet[1];
            int16_t raw_y = mouse_packet[2];

            if (mouse_packet[0] & 0x10) raw_x |= 0xFF00;
            if (mouse_packet[0] & 0x20) raw_y |= 0xFF00;

            // Invertir Y para coordenadas estándar del sistema de ventanas
            raw_y = -raw_y;

            uint8_t btn = mouse_packet[0] & 0x07;

            uint8_t next = (mhead + 1) & (MOUSE_BUF_SIZE - 1);
            if (next != mtail) {
                mbuf[mhead].dx = raw_x;
                mbuf[mhead].dy = raw_y;
                mbuf[mhead].buttons = btn;
                mhead = next;
            }
            break;
    }
}

void ps2_init(void) {
    serial_puts("[PS2] Inicializando PS/2 controlador...\n");

    wait_input_buffer_empty();
    outb(0x64, 0xA8); // Habilitar dispositivo auxiliar
    drain_output();

    wait_input_buffer_empty();
    outb(0x64, 0x20);
    if (wait_output_buffer_full()) {
        uint8_t cmd = inb(0x60);
        cmd |= 0x03;   // IRQ1 e IRQ12 habilitados
        cmd &= ~0x30;  // Clocks habilitados
        wait_input_buffer_empty();
        outb(0x64, 0x60);
        wait_input_buffer_empty();
        outb(0x60, cmd);
    }

    int retries = 3, ack_ok = 0;
    while (retries--) {
        wait_input_buffer_empty();
        outb(0x64, 0xD4);
        wait_input_buffer_empty();
        outb(0x60, 0xF4); // Enable Data Reporting

        for (int i = 0; i < 10000; i++) {
            uint8_t s = inb(0x64);
            if ((s & 0x01) && (s & 0x20)) {
                if (inb(0x60) == 0xFA) { ack_ok = 1; break; }
            }
            io_wait();
        }
        if (ack_ok) break;
    }

    if (ack_ok) serial_puts("[PS2] Mouse inicializado correctamente (ACK 0xF4)\n");

    irq_install_handler(1, ps2_keyboard_irq);
    irq_install_handler(12, ps2_mouse_irq);

    ps2_start_thread();
}

int ps2_pop_scancode(uint8_t *out) {
    uint64_t flags = irq_save();
    if (ktail == khead) { irq_restore(flags); return 0; }
    *out = kbuf[ktail];
    ktail = (ktail + 1) & (KBD_BUF_SIZE - 1);
    irq_restore(flags);
    return 1;
}

int ps2_pop_mouse(int16_t *dx, int16_t *dy, uint8_t *buttons) {
    uint64_t flags = irq_save();
    if (mtail == mhead) { irq_restore(flags); return 0; }
    *dx = mbuf[mtail].dx;
    *dy = mbuf[mtail].dy;
    *buttons = mbuf[mtail].buttons;
    mtail = (mtail + 1) & (MOUSE_BUF_SIZE - 1);
    irq_restore(flags);
    return 1;
}

void ps2_process(void) {
    if (ps2_has_scancode() || ps2_has_mouse()) {
        compositor_notify_event();
    }
}

static void ps2_thread(void) {
    while (1) {
        ps2_process();
        sched_yield(); // Cedemos CPU en lugar de colgar con hlt
    }
}

void ps2_start_thread(void) {
    sched_create_task(ps2_thread);
}

int ps2_has_scancode(void) { return ktail != khead; }
int ps2_has_mouse(void) { return mtail != mhead; }