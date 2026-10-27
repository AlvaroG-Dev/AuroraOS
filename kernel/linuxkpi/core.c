// SPDX-License-Identifier: GPL-2.0-only
/*
 * linuxkpi.kur: capa de compatibilidad con drivers de Linux para Aurora.
 * Punto de entrada del modulo, log (printk), avisos (WARN/BUG) y panic.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <lkpi/kur_abi.h>

#include "aurora.h"
#include "lkpi_internal.h"
#include "fs/fs.h"

LKPI_KUR_MANIFEST("linuxkpi");

/* ------------------------------------------------------------------ log */

/* Nivel de Linux (0..7) -> nivel de klog de Aurora. */
static int lkpi_klog_level(int lvl)
{
	if (lvl <= LOGLEVEL_ERR)
		return AURORA_KLOG_ERR;
	if (lvl == LOGLEVEL_WARNING)
		return AURORA_KLOG_WARN;
	if (lvl == LOGLEVEL_DEBUG)
		return AURORA_KLOG_DEBUG;
	return AURORA_KLOG_INFO;
}

static void lkpi_log_str(int klvl, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	aurora_klog_vprintf(klvl, fmt, ap);
	va_end(ap);
}

int lkpi_vprintk(const char *fmt, va_list args)
{
	char buf[512];
	int lvl = LOGLEVEL_DEFAULT;
	int n;

	while (fmt[0] == KERN_SOH_ASCII && fmt[1]) {
		if (fmt[1] >= '0' && fmt[1] <= '7')
			lvl = fmt[1] - '0';
		fmt += 2;
	}
	n = vscnprintf(buf, sizeof(buf), fmt, args);
	/* klog no termina la linea: se anade si el mensaje no la trae. */
	lkpi_log_str(lkpi_klog_level(lvl == LOGLEVEL_DEFAULT ? LOGLEVEL_INFO : lvl),
		     (n > 0 && buf[n - 1] == '\n') ? "%s" : "%s\n", buf);
	return n;
}
EXPORT_SYMBOL(lkpi_vprintk);

int lkpi_printk(const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = lkpi_vprintk(fmt, ap);
	va_end(ap);
	return n;
}
EXPORT_SYMBOL(lkpi_printk);

void lkpi_print_hex_dump(const char *level, const char *prefix_str, int prefix_type,
			 int rowsize, int groupsize, const void *buf, size_t len, bool ascii)
{
	const u8 *p = buf;
	char line[200];
	size_t i, j;

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;
	for (i = 0; i < len; i += rowsize) {
		int n = 0;
		if (prefix_type == DUMP_PREFIX_ADDRESS)
			n += scnprintf(line + n, sizeof(line) - n, "%p: ", p + i);
		else if (prefix_type == DUMP_PREFIX_OFFSET)
			n += scnprintf(line + n, sizeof(line) - n, "%.8zx: ", i);
		for (j = 0; j < (size_t)rowsize && i + j < len; j++)
			n += scnprintf(line + n, sizeof(line) - n, "%02x ", p[i + j]);
		if (ascii) {
			n += scnprintf(line + n, sizeof(line) - n, " ");
			for (j = 0; j < (size_t)rowsize && i + j < len; j++) {
				u8 c = p[i + j];
				n += scnprintf(line + n, sizeof(line) - n, "%c",
					       (c >= 32 && c < 127) ? c : '.');
			}
		}
		lkpi_printk("%s%s%s\n", level, prefix_str, line);
	}
}
EXPORT_SYMBOL(lkpi_print_hex_dump);

/* ----------------------------------------------------- avisos y fallos */

void lkpi_warn_slowpath(const char *file, int line, const char *fmt, ...)
{
	lkpi_printk(KERN_WARNING "WARNING: %s:%d\n", file, line);
	if (fmt) {
		va_list ap;
		va_start(ap, fmt);
		lkpi_vprintk(fmt, ap);
		va_end(ap);
	}
}
EXPORT_SYMBOL(lkpi_warn_slowpath);

void lkpi_bug(const char *file, int line)
{
	aurora_panic_noctx("[linuxkpi] BUG en %s:%d", file, line);
}
EXPORT_SYMBOL(lkpi_bug);

void lkpi_panic(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vscnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	aurora_panic_noctx("[linuxkpi] %s", buf);
}
EXPORT_SYMBOL(lkpi_panic);

void lkpi_dump_stack(void)
{
	unsigned long *fp = __builtin_frame_address(0);
	int depth;

	lkpi_printk(KERN_WARNING "linuxkpi: pila de llamadas:\n");
	for (depth = 0; fp && depth < 16; depth++) {
		unsigned long ret = fp[1];
		if (!ret || (unsigned long)fp < 0xFFFF800000000000UL)
			break;
		lkpi_printk(KERN_WARNING "  [%d] %px\n", depth, (void *)ret);
		fp = (unsigned long *)fp[0];
	}
}
EXPORT_SYMBOL(lkpi_dump_stack);

/* ------------------------------------------------------- planificador */

void lkpi_preempt_disable(void)
{
	aurora_preempt_disable();
}
EXPORT_SYMBOL(lkpi_preempt_disable);

void lkpi_preempt_enable(void)
{
	aurora_preempt_enable();
}
EXPORT_SYMBOL(lkpi_preempt_enable);

/* ------------------------------------------------------------- tiempo */

unsigned long lkpi_jiffies(void)
{
	return (unsigned long)aurora_sched_get_ticks();	/* HZ = 1000 */
}
EXPORT_SYMBOL(lkpi_jiffies);

/* ktime: kernel/time.c (TSC). */

/* ------------------------------------------------------- init / exit */

int kur_init(void);
void kur_exit(void);

int kur_init(void)
{
	int rc = lkpi_mm_init();
	if (rc)
		return rc;
	rc = lkpi_slab_init();
	if (rc)
		goto err_mm;
	rc = lkpi_sched_init();
	if (rc)
		goto err_slab;
	lkpi_time_init();
	lkpi_wait_init();
	rc = lkpi_softirq_init();
	if (rc)
		goto err_sched;
	rc = lkpi_rcu_init();
	if (rc)
		goto err_softirq;
	rc = lkpi_wq_init();
	if (rc)
		goto err_rcu;
	rc = lkpi_device_core_init();
	if (rc)
		goto err_wq;
	rc = lkpi_platform_bus_init();
	if (rc)
		goto err_core;
	rc = lkpi_pci_init();
	if (rc)
		goto err_platform;
	rc = lkpi_chrdev_init();
	if (rc)
		goto err_pci;
	rc = lkpi_dmabuf_init();
	if (rc)
		goto err_chrdev;
	rc = lkpi_debugfs_init();
	if (rc)
		goto err_dmabuf;
	rc = lkpi_i2c_init();
	if (rc)
		goto err_debugfs;
	rc = lkpi_backlight_init();
	if (rc)
		goto err_i2c;
	pr_info("linuxkpi: capa de compatibilidad con drivers de Linux lista\n");
	return 0;

err_i2c:
	lkpi_i2c_exit();
err_debugfs:
	lkpi_debugfs_exit();
err_dmabuf:
	lkpi_dmabuf_exit();
err_chrdev:
	lkpi_chrdev_exit();
err_pci:
	lkpi_pci_exit();
err_platform:
	lkpi_platform_bus_exit();
err_core:
	lkpi_device_core_exit();
err_wq:
	lkpi_wq_exit();
err_rcu:
	lkpi_rcu_exit();
err_softirq:
	lkpi_softirq_exit();
err_sched:
	lkpi_sched_exit();
err_slab:
	lkpi_slab_exit();
err_mm:
	lkpi_mm_exit();
	return rc;
}

void kur_exit(void)
{
	/* El modelo de dispositivos antes que las workqueues y RCU que usa. */
	lkpi_backlight_exit();
	lkpi_i2c_exit();
	lkpi_debugfs_exit();
	lkpi_dmabuf_exit();
	lkpi_chrdev_exit();
	lkpi_pci_exit();
	lkpi_platform_bus_exit();
	lkpi_device_core_exit();
	/* Primero lo que encola trabajo o callbacks; RCU vacia su cola al salir. */
	lkpi_wq_exit();
	lkpi_rcu_exit();
	lkpi_softirq_exit();
	lkpi_sched_exit();
	lkpi_ioremap_exit();
	lkpi_vmalloc_exit();
	lkpi_slab_exit();
	lkpi_mm_exit();
	pr_info("linuxkpi: descargada\n");
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Aurora LinuxKPI");
