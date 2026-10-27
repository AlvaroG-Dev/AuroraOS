/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Descriptores de fichero del proceso actual (K8, L4). fdget() de un fd que
 * no es de la capa devuelve un struct fd vacio, salvo los dmabuf de Aurora,
 * que solo entiende dma_buf_get().
 */
#ifndef _LKPI_LINUX_FILE_H
#define _LKPI_LINUX_FILE_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cleanup.h>

struct fd {
	struct file *file;
	void *__lkpi_afd;		/* file_descriptor_t con referencia */
};

struct fd lkpi_fdget(unsigned int fd);
void lkpi_fdput(struct fd f);
struct file *lkpi_fget(unsigned int fd);
int lkpi_get_unused_fd_flags(unsigned int flags);
void lkpi_put_unused_fd(unsigned int fd);
void lkpi_fd_install(unsigned int fd, struct file *file);

#define fdget(fd) lkpi_fdget(fd)
#define fdput(f) lkpi_fdput(f)
#define fd_file(f) ((f).file)
#define fd_empty(f) (!(f).file)
#define fget(fd) lkpi_fget(fd)
#define get_unused_fd_flags(f) lkpi_get_unused_fd_flags(f)
#define put_unused_fd(fd) lkpi_put_unused_fd(fd)
#define fd_install(fd, f) lkpi_fd_install(fd, f)
DEFINE_CLASS(fd, struct fd, lkpi_fdput(_T), lkpi_fdget(fd), int fd)

#endif
