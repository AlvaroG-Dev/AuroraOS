// tests/uc_shim.h — en la prueba con Unicorn, main.c usa el /dev/aurora-vm de mentira.
#ifndef AVM_UC_SHIM_H
#define AVM_UC_SHIM_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef AVM_UC_BACKEND
#include <fcntl.h>
#include <sys/ioctl.h>
int avm_test_open(const char *path, int flags, ...);
int avm_test_ioctl(int fd, unsigned long req, ...);
#define open avm_test_open
#define ioctl avm_test_ioctl
#endif
#endif
