#ifndef _LKPI_LINUX_LEDS_H
#define _LKPI_LINUX_LEDS_H
#include <linux/types.h>
#include <linux/list.h>
struct led_classdev { const char *name; int brightness; };
#endif
