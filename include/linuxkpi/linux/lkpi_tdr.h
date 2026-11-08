/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Watchdog de GPU (TDR, Timeout Detection and Recovery) de la capa (L9.4).
 *
 * Un driver (o el pegamento de Aurora) registra un `lkpi_tdr` con tres
 * funciones: `busy` dice si hay trabajo pendiente, `progress` devuelve un
 * contador que avanza cada vez que se completa trabajo y `reset` intenta
 * recuperar el dispositivo. Si hay trabajo pendiente y el contador no avanza
 * durante `timeout_ms`, se declara un cuelgue y se llama a `reset` (por
 * omision, el reset de la funcion PCI). Tras `max_resets` recuperaciones en
 * `window_ms` el dispositivo se da por perdido ("wedged"): se deja de
 * reiniciar y se avisa por uevent para no entrar en un bucle de reinicios.
 *
 * i915 y NVIDIA mantienen su propio hangcheck; esto es para los drivers que
 * no lo tienen y como red de seguridad comun (sondeo cada timeout/4).
 */
#ifndef _LKPI_LINUX_TDR_H
#define _LKPI_LINUX_TDR_H

#include <linux/device.h>
#include <linux/workqueue.h>

struct lkpi_tdr {
	const char *name;
	struct device *dev;
	unsigned int timeout_ms;	/* defecto 2000 */
	unsigned int max_resets;	/* defecto 3 */
	unsigned int window_ms;		/* defecto 60000 */
	bool (*busy)(void *priv);
	u64 (*progress)(void *priv);
	int (*reset)(void *priv);	/* NULL: reset de la funcion PCI */
	void *priv;

	/* estadisticas (solo lectura) */
	unsigned int hangs, resets, failed_resets;
	bool wedged;

	/* internos */
	struct delayed_work work;
	unsigned long stamp;		/* ultimo avance visto */
	u64 last_progress;
	unsigned long window_start;
	unsigned int window_resets;
	bool active;
};

int lkpi_tdr_register(struct lkpi_tdr *t);
void lkpi_tdr_unregister(struct lkpi_tdr *t);
/* Reset de la funcion PCI del dispositivo (FLR o reset por PM). */
int lkpi_tdr_default_reset(struct device *dev);

#endif
