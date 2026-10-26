// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lo que el cargador .kur de Aurora debe soportar para alojar drivers de
 * Linux (Plan_LinuxKPI.md, L0.4). El lado del kernel (kernel_tests.c)
 * comprueba ademas la exportacion de nombre largo y .fini_array.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include "lkpi_test.h"

/* 1) Muchas secciones: con -ffunction-sections cada funcion lleva la suya
 * (y su .rela), muy por encima de las 64 de antes. */
#define F(n) noinline int lkpi_tl_f##n(int x) { return x + n; }
#define F10(n) F(n##0) F(n##1) F(n##2) F(n##3) F(n##4) F(n##5) F(n##6) F(n##7) F(n##8) F(n##9)
F10(1) F10(2) F10(3) F10(4) F10(5) F10(6) F10(7) F10(8)

/* 2) Constructores (.init_array) */
static int ctor_runs;
static __attribute__((constructor)) void tl_ctor(void)
{
	ctor_runs++;
}

/* 3) .fini_array: el kernel comprueba que se ejecuta al descargar. */
extern volatile int kur_test_fini_counter;
static __attribute__((destructor)) void tl_dtor(void)
{
	kur_test_fini_counter++;
}

/* 4) __start_SEC / __stop_SEC de una seccion propia */
struct tl_item { long v; };	/* de 8 bytes: sin huecos con __aligned(8) */
#define TL_ITEM(n, val) \
	static const struct tl_item __tl_item_##n __used __section("lkpi_tl_set") \
		__aligned(8) = { val }
TL_ITEM(a, 1);
TL_ITEM(b, 10);
TL_ITEM(c, 100);
extern const struct tl_item __start_lkpi_tl_set[], __stop_lkpi_tl_set[];

/* 5) Simbolo debil sin definicion: 0 */
extern int lkpi_tl_does_not_exist __attribute__((weak));

/* 6) .bss grande: imagen de mas de 16 MiB (el antiguo KUR_MAX_SIZE). */
static char big_bss[24 << 20];

/* 7) Exportacion con nombre de mas de 64 caracteres. */
int lkpi_test_loader_exported_function_with_a_name_longer_than_sixty_four_chars(void);
int lkpi_test_loader_exported_function_with_a_name_longer_than_sixty_four_chars(void)
{
	return 0x4C4B5049;	/* "LKPI" */
}
EXPORT_SYMBOL(lkpi_test_loader_exported_function_with_a_name_longer_than_sixty_four_chars);

int lkpi_tl_target = 1234;

/* 8) R_X86_64_PC64: diferencia de 64 bits entre un dato y un simbolo. */
extern const long lkpi_tl_pc64;
__asm__(".pushsection .data.lkpi_tl_pc64,\"aw\"\n"
	".balign 8\n"
	".globl lkpi_tl_pc64\n"
	"lkpi_tl_pc64: .quad lkpi_tl_target - .\n"
	".popsection");

static int __init test_loader_init(void)
{
	void *got_local, *got_import;
	int sum = 0, n = 0;
	const struct tl_item *it;

	KT_EQ(lkpi_tl_f11(0) + lkpi_tl_f89(0), 11 + 89);
	KT_EQ(ctor_runs, 1);

	for (it = __start_lkpi_tl_set; it < __stop_lkpi_tl_set; it++) {
		sum += it->v;
		n++;
	}
	KT_EQ(n, 3);
	KT_EQ(sum, 111);

	KT_ASSERT(&lkpi_tl_does_not_exist == NULL, "simbolo debil resuelto a %px",
		  &lkpi_tl_does_not_exist);

	/* 9) GOTPCREL: a un simbolo propio y a uno importado de linuxkpi.kur */
	__asm__("movq lkpi_tl_target@GOTPCREL(%%rip), %0" : "=r"(got_local));
	__asm__("movq lkpi_printk@GOTPCREL(%%rip), %0" : "=r"(got_import));
	KT_ASSERT(got_local == &lkpi_tl_target, "GOTPCREL local %px != %px", got_local,
		  &lkpi_tl_target);
	KT_ASSERT(got_import == (void *)lkpi_printk, "GOTPCREL importado %px != %px",
		  got_import, (void *)lkpi_printk);

	KT_ASSERT((long)&lkpi_tl_pc64 + lkpi_tl_pc64 == (long)&lkpi_tl_target,
		  "PC64: %lx", lkpi_tl_pc64);

	big_bss[0] = 1;
	big_bss[sizeof(big_bss) - 1] = 2;
	KT_EQ(big_bss[0] + big_bss[sizeof(big_bss) - 1], 3);
	KT_EQ(big_bss[sizeof(big_bss) / 2], 0);

	pr_info("lkpi_test_loader: secciones, constructores, GOT y PC64 comprobados\n");
	return 0;
}

static void __exit test_loader_exit(void)
{
}

module_init(test_loader_init);
module_exit(test_loader_exit);
MODULE_LICENSE("GPL");
