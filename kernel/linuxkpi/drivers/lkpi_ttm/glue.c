// SPDX-License-Identifier: GPL-2.0-only
/*
 * lkpi_ttm.kur: punto de entrada del modulo. Linux registra el slab del
 * buddy y el de los fences del planificador con module_init() en cada
 * fichero; aqui se renombran (lkpi_ttm.mk) y se llaman en orden desde un
 * unico kur_init. TTM no tiene module_init: arranca en ttm_device_init().
 */
#include <linux/init.h>
#include <linux/module.h>

int lkpi_gpu_buddy_init(void);
void lkpi_gpu_buddy_exit(void);
int lkpi_sched_fence_init(void);
void lkpi_sched_fence_exit(void);

static int __init lkpi_ttm_init(void)
{
	int ret = lkpi_gpu_buddy_init();

	if (ret)
		return ret;
	ret = lkpi_sched_fence_init();
	if (ret)
		lkpi_gpu_buddy_exit();
	return ret;
}

static void __exit lkpi_ttm_exit(void)
{
	lkpi_sched_fence_exit();
	lkpi_gpu_buddy_exit();
}

module_init(lkpi_ttm_init);
module_exit(lkpi_ttm_exit);
MODULE_DESCRIPTION("TTM, drm_buddy y gpu-sched de Linux sobre LinuxKPI");
MODULE_LICENSE("GPL");
