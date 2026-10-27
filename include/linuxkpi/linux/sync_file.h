/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sync_file (K7, L5): un fd que representa un fence. En la capa es un fichero
 * de la capa (anon_inode) con poll e ioctl; sync_file_get_fence() acepta ademas
 * los fds de sync_file de Aurora (los de DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD y
 * DMA_BUF_IOCTL_EXPORT_SYNC_FILE), que se envuelven en un dma_fence.
 */
#ifndef _LKPI_LINUX_SYNC_FILE_H
#define _LKPI_LINUX_SYNC_FILE_H

#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>

#define sync_file_create lkpi_sync_file_create
#define sync_file_get_fence lkpi_sync_file_get_fence
#define sync_file_get_name lkpi_sync_file_get_name

struct file;

struct sync_file {
	struct file		*file;
	char			user_name[32];
	wait_queue_head_t	wq;
	unsigned long		flags;
	struct dma_fence	*fence;
	struct dma_fence_cb cb;
};

#define POLL_ENABLED 0

struct sync_file *sync_file_create(struct dma_fence *fence);
struct dma_fence *sync_file_get_fence(int fd);
char *sync_file_get_name(struct sync_file *sync_file, char *buf, int len);

#endif
