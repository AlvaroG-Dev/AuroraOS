// kernel/driver.c
#include "driver.h"
#include "klog.h"
#include <stddef.h>

#define MAX_DRIVERS 32

static struct driver *drivers[MAX_DRIVERS];
static int driver_count = 0;

void driver_register(struct driver *d) {
  if (!d) {
    LOG_ERR("[DRIVER] driver nulo");
    return;
  }
  if (driver_count >= MAX_DRIVERS) {
    LOG_ERR("[DRIVER] registro lleno (max=%u)", MAX_DRIVERS);
    return;
  }
  drivers[driver_count++] = d;
}

int drivers_init_all(void) {
  int failures = 0;
  for (int i = 0; i < driver_count; i++) {
    LOG_INFO("[DRIVER] Inicializando '%s'...", drivers[i]->name);
    if (drivers[i]->init) {
      int ret = drivers[i]->init();
      if (ret != 0) {
        LOG_ERR("[DRIVER] '%s' falló con %d", drivers[i]->name, ret);
        failures++;
      }
    }
  }
  return failures ? -1 : 0;
}

void drivers_shutdown_all(void) {
  for (int i = driver_count - 1; i >= 0; i--) {
    if (drivers[i]->shutdown) {
      LOG_INFO("[DRIVER] Apagando '%s'...", drivers[i]->name);
      drivers[i]->shutdown();
    }
  }
}