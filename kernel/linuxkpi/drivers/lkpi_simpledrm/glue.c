// SPDX-License-Identifier: GPL-2.0-only
/*
 * lkpi_simpledrm.kur: punto de entrada. Registra el driver simpledrm de Linux
 * y crea el dispositivo "simple-framebuffer" del GOP (lo que en Linux hace
 * sysfb a partir de screen_info), con la direccion y el modo que el kernel de
 * Aurora recibio del firmware.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>

int lkpi_simpledrm_driver_init(void);
void lkpi_simpledrm_driver_exit(void);

int lkpi_gop_framebuffer(u64 *phys, u64 *size) __asm__("gpup_get_framebuffer");
int lkpi_gop_mode(u32 *w, u32 *h, u32 *pitch, u32 *bpp) __asm__("gpup_get_fb_mode");

static struct platform_device *gop_pdev;

static int __init lkpi_simpledrm_init(void)
{
	struct simplefb_platform_data pd;
	struct resource res = { .flags = IORESOURCE_MEM, .name = "aurora-gop" };
	struct platform_device_info info = { .name = "simple-framebuffer", .id = PLATFORM_DEVID_NONE };
	u64 phys, size;
	u32 w, h, pitch, bpp;
	int ret;

	ret = lkpi_simpledrm_driver_init();
	if (ret)
		return ret;
	if (!lkpi_gop_framebuffer(&phys, &size) || !lkpi_gop_mode(&w, &h, &pitch, &bpp)) {
		pr_warn("lkpi_simpledrm: el firmware no dejo framebuffer\n");
		return 0;
	}
	pd.width = w;
	pd.height = h;
	pd.stride = pitch;
	pd.format = bpp == 16 ? "r5g6b5" : bpp == 24 ? "r8g8b8" : "x8r8g8b8";
	res.start = phys;
	res.end = phys + size - 1;
	info.res = &res;
	info.num_res = 1;
	info.data = &pd;
	info.size_data = sizeof(pd);
	gop_pdev = platform_device_register_full(&info);
	if (IS_ERR(gop_pdev)) {
		ret = PTR_ERR(gop_pdev);
		gop_pdev = NULL;
		lkpi_simpledrm_driver_exit();
		return ret;
	}
	return 0;
}

static void __exit lkpi_simpledrm_exit(void)
{
	if (gop_pdev)
		platform_device_unregister(gop_pdev);
	lkpi_simpledrm_driver_exit();
}

module_init(lkpi_simpledrm_init);
module_exit(lkpi_simpledrm_exit);
MODULE_DESCRIPTION("simpledrm de Linux sobre LinuxKPI (framebuffer del GOP)");
MODULE_LICENSE("GPL");
