// kernel/input_proxy.h
#ifndef KERNEL_INPUT_PROXY_H
#define KERNEL_INPUT_PROXY_H

#include "vfs.h"

// VFS ops de /dev/inputproxy. Se registran en vfs.c (devfs).
extern vfs_ops_t inputp_dev_ops;

#endif
