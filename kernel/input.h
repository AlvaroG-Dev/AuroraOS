// kernel/input.h
#ifndef KERNEL_INPUT_H
#define KERNEL_INPUT_H

#include <stddef.h>
#include <stdint.h>

#define INPUT_BUF_SIZE 256

// Tipos de evento
#define INPUT_EV_KEY 1 // Teclado: code=scancode, value=1 (press) / 0 (release)
#define INPUT_EV_MOUSE 2 // Ratón: code=botones, value=dx, value2=dy

typedef struct input_event {
  uint8_t type;
  uint8_t code;
  int16_t value;
  int16_t value2;
  uint32_t timestamp; // tick_count
} input_event_t;

void input_init(void);

// Empujar un evento (llamado desde drivers/tareas).
void input_push(const input_event_t *ev);

// Sacar un evento (no bloqueante). Devuelve 1 si hay evento, 0 si no.
int input_pop(input_event_t *out);

// Sacar un evento bloqueante. Duerme hasta que haya uno.
// Retorna 0 si obtuvo evento, -EINTR si fue cancelada.
int input_wait_event(input_event_t *out);

// Cuántos eventos hay en cola.
size_t input_pending(void);

#endif