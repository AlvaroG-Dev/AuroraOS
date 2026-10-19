/* kernel/boot_splash.h */
#ifndef BOOT_SPLASH_H
#define BOOT_SPLASH_H

#include <stdint.h>

/* 0 = log de texto (boot_console), 1 = animación. Editable en caliente. */
extern int g_boot_splash_enabled;

typedef void (*boot_splash_render_fn_t)(uint32_t *fb, uint32_t w, uint32_t h,
                                        uint32_t pitch);

/* Devuelve 0 si todo OK, -1 si no se pudo inicializar (sin font o sin
 * memoria para la sombra). En ese caso el llamador debe caer a
 * boot_console_init(). NO pinta nada en el framebuffer hasta el primer
 * boot_splash_tick(). */
int boot_splash_init(uint32_t *fb, uint32_t width, uint32_t height,
                     uint32_t pitch_px);

/* Ajusta la frecuencia de TSC (llamar tras klog_calibrate_tsc). */
void boot_splash_set_tsc_hz(uint64_t hz);

int boot_splash_active(void);

/* Avanza la animación. Idempotente y throttled internamente a ~60 fps. */
void boot_splash_tick(void);

/* Cuerpo del task dedicado. Corre hasta que la splash pasa a reveal o
 * se cancela. Pensado para sched_create_task(). */
void boot_splash_task_loop(void);

/* Transición slide-up al escritorio.
 * render_below: si != NULL, se llama para dibujar el escritorio ANTES
 * de la animación. Si es NULL, se rellena con COL_DESKTOP. */
void boot_splash_begin_reveal(boot_splash_render_fn_t render_below);

/* Bombea el reveal. Devuelve 1 mientras dura, 0 cuando termina. */
int boot_splash_tick_reveal(void);

/* Libera y desactiva. Llamar al terminar. */
void boot_splash_finish(void);

#endif