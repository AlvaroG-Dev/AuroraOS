/* faux bus: cfg80211 la usa para el dispositivo "regulatory". */
#ifndef _LKPI_DEVICE_FAUX_H
#define _LKPI_DEVICE_FAUX_H
#include <linux/device.h>
struct faux_device { struct device dev; };
struct faux_device_ops { int (*probe)(struct faux_device *fdev); void (*remove)(struct faux_device *fdev); };
struct faux_device *faux_device_create(const char *name, struct device *parent, const struct faux_device_ops *faux_ops);
void faux_device_destroy(struct faux_device *faux_dev);
#define faux_device_get_drvdata(f) dev_get_drvdata(&(f)->dev)
#endif
