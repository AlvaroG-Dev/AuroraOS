/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Los "bottom halves" de la capa (timers, tasklets, irq_work) corren en el
 * hilo lkpi_softirqd, que no puede expulsar a una tarea con la expulsion
 * desactivada en su CPU: basta con desactivarla.
 */
#ifndef _LKPI_LINUX_BOTTOM_HALF_H
#define _LKPI_LINUX_BOTTOM_HALF_H

#include <linux/preempt.h>

#define local_bh_disable() preempt_disable()
#define local_bh_enable() preempt_enable()
#define local_bh_enable_ip(ip) preempt_enable()

#endif
