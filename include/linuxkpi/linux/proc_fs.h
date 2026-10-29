/* SPDX-License-Identifier: GPL-2.0-only */
/* /proc para drivers (N2): Aurora no tiene procfs del lado kernel; las entradas
 * no se crean (NULL) y los drivers lo toleran como "procfs deshabilitado". */
#ifndef _LKPI_LINUX_PROC_FS_H
#define _LKPI_LINUX_PROC_FS_H
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/seq_file.h>
#include <linux/cdev.h>

struct proc_dir_entry;
static inline struct proc_dir_entry *proc_mkdir_mode(const char *n, umode_t m, struct proc_dir_entry *p) { return NULL; }
static inline struct proc_dir_entry *proc_mkdir(const char *n, struct proc_dir_entry *p) { return NULL; }
static inline struct proc_dir_entry *proc_create_data(const char *n, umode_t m, struct proc_dir_entry *p,
						      const struct file_operations *f, void *d) { return NULL; }
static inline struct proc_dir_entry *proc_create(const char *n, umode_t m, struct proc_dir_entry *p,
						 const struct file_operations *f) { return NULL; }
static inline void remove_proc_entry(const char *n, struct proc_dir_entry *p) {}
static inline void proc_remove(struct proc_dir_entry *p) {}
static inline void *pde_data(const struct inode *i) { return NULL; }
#define PDE_DATA(i) pde_data(i)
#endif
