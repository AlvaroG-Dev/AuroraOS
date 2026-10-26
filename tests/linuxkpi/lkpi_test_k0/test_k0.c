// SPDX-License-Identifier: GPL-2.0-only
/* Pruebas de K0 (base C): formatos, conversiones, bits, atomicos, tiempo. */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/bsearch.h>
#include <linux/crc32.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/jhash.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/unaligned.h>
#include "lkpi_test.h"

struct va_format {
	const char *fmt;
	va_list *va;
};

static char *fmt_va(char *buf, size_t n, const char *fmt, ...)
{
	struct va_format vaf;
	va_list ap;
	va_start(ap, fmt);
	vaf.fmt = fmt;
	vaf.va = &ap;
	snprintf(buf, n, "<%pV>", &vaf);
	va_end(ap);
	return buf;
}

static void test_printf(void)
{
	char buf[128];
	u8 ip[4] = { 192, 168, 1, 7 };
	u8 mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
	u8 hex[4] = { 0xde, 0xad, 0xbe, 0xef };
	phys_addr_t pa = 0xfee00000;
	u32 fourcc = 0x34325258;	/* "XR24" */

	snprintf(buf, sizeof(buf), "%d %u %x %X %o %c %s", -42, 42u, 0xabcu, 0xabcu, 8, 'z', "ok");
	KT_STREQ(buf, "-42 42 abc ABC 10 z ok");
	snprintf(buf, sizeof(buf), "[%5d][%-5d][%05d][%.3s][%+d][% d][%#x][%#o]",
		 42, 42, 42, "abcdef", 7, 7, 255, 8);
	KT_STREQ(buf, "[   42][42   ][00042][abc][+7][ 7][0xff][010]");
	snprintf(buf, sizeof(buf), "%lld %llu %llx %zu %zd %ld %hhu %hd",
		 -9000000000LL, 18446744073709551615ULL, 0x123456789abcULL, (size_t)77,
		 (ssize_t)-3, -1L, 300, (short)-2);
	KT_STREQ(buf, "-9000000000 18446744073709551615 123456789abc 77 -3 -1 44 -2");
	snprintf(buf, sizeof(buf), "%*d|%-*s|%.*s", 4, 1, 3, "a", 2, "xyz");
	KT_STREQ(buf, "   1|a  |xy");
	snprintf(buf, sizeof(buf), "%pe %pe", ERR_PTR(-ENOMEM), ERR_PTR(-EPROBE_DEFER));
	KT_STREQ(buf, "-ENOMEM -EPROBE_DEFER");
	snprintf(buf, sizeof(buf), "%pI4 %pi4 %pM %pm", ip, ip, mac, mac);
	KT_STREQ(buf, "192.168.1.7 192.168.001.007 52:54:00:12:34:56 525400123456");
	snprintf(buf, sizeof(buf), "%*ph|%*phC|%*phN", 4, hex, 2, hex, 3, hex);
	KT_STREQ(buf, "de ad be ef|de:ad|deadbe");
	snprintf(buf, sizeof(buf), "%pa", &pa);
	KT_STREQ(buf, "0x00000000fee00000");
	snprintf(buf, sizeof(buf), "%px", (void *)0xffff800000001000UL);
	KT_STREQ(buf, "ffff800000001000");
	snprintf(buf, sizeof(buf), "%p4cc", &fourcc);
	KT_STREQ(buf, "XR24 little-endian (0x34325258)");
	KT_STREQ(fmt_va(buf, sizeof(buf), "%s=%d", "x", 5), "<x=5>");
	{
		char *volatile nullstr = NULL;
		snprintf(buf, sizeof(buf), "%s", nullstr);
	}
	KT_STREQ(buf, "(null)");

	/* Truncado y valores de retorno */
	KT_EQ(snprintf(buf, 4, "abcdef"), 6);
	KT_STREQ(buf, "abc");
	KT_EQ(scnprintf(buf, 4, "abcdef"), 3);
	KT_EQ(snprintf(NULL, 0, "%d", 12345), 5);
	KT_EQ(sprintf(buf, "%u-%u", 1, 2), 3);
	KT_STREQ(buf, "1-2");
	{
		char *s = kasprintf(GFP_KERNEL, "kasprintf %d %s", 9, "fin");
		KT_STREQ(s, "kasprintf 9 fin");
		kfree(s);
	}
	/* printk con niveles (solo que no falle) */
	pr_info("lkpi_test_k0: printk %s %d %pe\n", "nivel", 6, ERR_PTR(-EINVAL));
	printk(KERN_DEBUG "lkpi_test_k0: KERN_DEBUG\n");
}

static void test_conv(void)
{
	unsigned long ul;
	long l;
	int i;
	unsigned int u;
	u8 u8v = 0;
	bool b;
	char *end;
	int a, c;
	unsigned int x;
	char word[8];

	KT_EQ(kstrtoul("4096", 0, &ul), 0);
	KT_EQ(ul, 4096);
	KT_EQ(kstrtoul("0x1F\n", 0, &ul), 0);
	KT_EQ(ul, 31);
	KT_EQ(kstrtoul("017", 0, &ul), 0);
	KT_EQ(ul, 15);
	KT_EQ(kstrtol("-123", 10, &l), 0);
	KT_EQ(l, -123);
	KT_EQ(kstrtoint("2147483648", 10, &i), -ERANGE);
	KT_EQ(kstrtouint("12abc", 10, &u), -EINVAL);
	KT_EQ(kstrtou8("256", 10, &u8v), -ERANGE);
	KT_EQ(kstrtou8("ff", 16, &u8v), 0);
	KT_EQ(u8v, 255);
	KT_EQ(kstrtoul("18446744073709551616", 10, &ul), -ERANGE);
	KT_EQ(kstrtobool("on", &b), 0);
	KT_EQ(b, 1);
	KT_EQ(kstrtobool("N", &b), 0);
	KT_EQ(b, 0);
	KT_EQ(simple_strtoul("0x40xyz", &end, 0), 0x40);
	KT_STREQ(end, "xyz");
	KT_EQ(sscanf("12 0x1f word", "%d %x %7s", &a, &x, word), 3);
	KT_EQ(a, 12);
	KT_EQ(x, 0x1f);
	KT_STREQ(word, "word");
	KT_EQ(sscanf("7,-8", "%d,%d", &a, &c), 2);
	KT_EQ(c, -8);
}

static void test_bits(void)
{
	DECLARE_BITMAP(bm, 200);
	DECLARE_BITMAP(bm2, 200);
	unsigned long w = 0, bit, n;
	u32 reg;

	KT_EQ(BIT(5), 32);
	KT_EQ(GENMASK(7, 4), 0xf0);
	KT_EQ(GENMASK_ULL(63, 62), 0xC000000000000000ULL);
	reg = FIELD_PREP(GENMASK(11, 8), 0xA) | 0x5;
	KT_EQ(reg, 0xA05);
	KT_EQ(FIELD_GET(GENMASK(11, 8), reg), 0xA);
	KT_EQ(FIELD_FIT(GENMASK(3, 0), 16), 0);
	KT_EQ(u32_get_bits(0xA05, GENMASK(11, 8)), 0xA);

	set_bit(3, &w);
	__set_bit(63, &w);
	KT_ASSERT(test_bit(3, &w) && test_bit(63, &w) && !test_bit(4, &w), "set_bit");
	KT_EQ(test_and_set_bit(3, &w), 1);
	KT_EQ(test_and_clear_bit(3, &w), 1);
	KT_EQ(test_bit(3, &w), 0);
	KT_EQ(__ffs(0x80UL), 7);
	KT_EQ(__fls(0x80UL), 7);
	KT_EQ(ffs(0), 0);
	KT_EQ(ffs(0x10), 5);
	KT_EQ(fls(0x10), 5);
	KT_EQ(fls64(1ULL << 40), 41);
	KT_EQ(ffz(0xffUL), 8);
	KT_EQ(hweight32(0xf0f0), 8);
	KT_EQ(hweight64(~0ULL), 64);
	KT_EQ(ilog2(4096), 12);
	KT_EQ(ilog2((unsigned long)w | 1), 63);
	KT_EQ(roundup_pow_of_two(33), 64);
	KT_EQ(rounddown_pow_of_two(33), 32);
	KT_EQ(is_power_of_2(64), 1);
	KT_EQ(is_power_of_2(0), 0);
	KT_EQ(order_base_2(5), 3);
	KT_EQ(rol32(0x80000001, 1), 3);
	KT_EQ(sign_extend32(0xff, 7), -1);

	bitmap_zero(bm, 200);
	bitmap_set(bm, 60, 10);
	KT_EQ(bitmap_weight(bm, 200), 10);
	KT_EQ(find_first_bit(bm, 200), 60);
	KT_EQ(find_next_zero_bit(bm, 200, 60), 70);
	KT_EQ(find_last_bit(bm, 200), 69);
	KT_EQ(find_nth_bit(bm, 200, 3), 63);
	n = 0;
	for_each_set_bit(bit, bm, 200)
		n += bit;
	KT_EQ(n, 645);
	bitmap_clear(bm, 62, 2);
	KT_EQ(bitmap_weight(bm, 200), 8);
	KT_EQ(bitmap_find_next_zero_area(bm, 200, 0, 70, 0), 70);
	bitmap_fill(bm2, 200);
	KT_EQ(bitmap_full(bm2, 200), 1);
	KT_EQ(bitmap_subset(bm, bm2, 200), 1);
	bitmap_andnot(bm2, bm2, bm, 200);
	KT_EQ(bitmap_intersects(bm, bm2, 200), 0);
	KT_EQ(bitmap_weight(bm2, 200), 192);
	KT_EQ(find_first_zero_bit(bm2, 200), 60);
}

static void test_atomic_math(void)
{
	atomic_t a = ATOMIC_INIT(5);
	atomic64_t a64 = ATOMIC64_INIT(1LL << 40);
	u64 v = 1000000007ULL;
	u32 rem;
	int x;

	KT_EQ(atomic_inc_return(&a), 6);
	KT_EQ(atomic_fetch_add(4, &a), 6);
	KT_EQ(atomic_read(&a), 10);
	KT_EQ(atomic_cmpxchg(&a, 10, 20), 10);
	KT_EQ(atomic_cmpxchg(&a, 10, 30), 20);
	KT_EQ(atomic_add_unless(&a, 1, 20), 0);
	atomic_set(&a, 1);
	KT_EQ(atomic_dec_and_test(&a), 1);
	KT_EQ(atomic_inc_not_zero(&a), 0);
	atomic64_add(5, &a64);
	KT_EQ(atomic64_read(&a64), (1LL << 40) + 5);
	x = 3;
	KT_EQ(xchg(&x, 9), 3);
	KT_EQ(cmpxchg(&x, 9, 11), 9);
	KT_EQ(x, 11);

	KT_EQ(min(3, 7), 3);
	KT_EQ(max_t(u8, 300, 2), 44);
	KT_EQ(clamp(15, 0, 10), 10);
	KT_EQ(DIV_ROUND_UP(10, 4), 3);
	KT_EQ(DIV_ROUND_CLOSEST(10, 4), 3);
	KT_EQ(round_up(13, 8), 16);
	KT_EQ(round_down(13, 8), 8);
	KT_EQ(ALIGN(4097, 4096), 8192);
	KT_EQ(IS_ALIGNED(8192, 4096), 1);
	KT_EQ(div_u64_rem(v, 1000, &rem), 1000000);
	KT_EQ(rem, 7);
	rem = do_div(v, 10);
	KT_EQ(v, 100000000);
	KT_EQ(rem, 7);
	KT_EQ(mul_u64_u32_div(1ULL << 40, 3, 4), 3ULL << 38);
	KT_EQ(mul_u64_u64_div_u64(1ULL << 62, 3, 6), 1ULL << 61);
	KT_EQ(int_sqrt(1000000), 1000);
	KT_EQ(gcd(12, 18), 6);
	KT_EQ(lcm(4, 6), 12);
	KT_EQ(upper_32_bits(0x1234567890ULL), 0x12);
	KT_EQ(lower_32_bits(0x1234567890ULL), 0x34567890);
	{
		size_t s;
		KT_EQ(check_add_overflow((u32)0xffffffff, (u32)1, (u32 *)&x), 1);
		KT_EQ(check_mul_overflow((size_t)1 << 62, (size_t)8, &s), 1);
		KT_EQ(array_size(SIZE_MAX / 2, 4), SIZE_MAX);
	}
}

struct kt_outer {
	int a;
	struct list_head inner;
	char tail[];
};

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static void test_misc(void)
{
	struct kt_outer o;
	u8 buf[8] = { 0 };
	int arr[64], key, *found;
	u64 seed = 12345;
	char s[32], *p, *tok;
	u8 bin[4];

	KT_ASSERT(container_of(&o.inner, struct kt_outer, inner) == &o, "container_of");
	KT_EQ(struct_size(&o, tail, 10), sizeof(o) + 10);
	KT_EQ(ARRAY_SIZE(arr), 64);
	KT_EQ(IS_ERR(ERR_PTR(-EIO)), 1);
	KT_EQ(PTR_ERR(ERR_PTR(-EIO)), -EIO);
	KT_EQ(IS_ERR_OR_NULL(NULL), 1);
	KT_EQ(PTR_ERR_OR_ZERO(&o), 0);
	KT_EQ(IS_ENABLED(CONFIG_X86_64), 1);
	KT_EQ(IS_ENABLED(CONFIG_DOES_NOT_EXIST), 0);
	KT_EQ(WARN_ON(0), 0);
	KT_EQ(WARN_ON_ONCE(1), 1);	/* imprime un aviso, a proposito */

	KT_EQ(cpu_to_be32(0x11223344), 0x44332211);
	KT_EQ(le16_to_cpu(0x1234), 0x1234);
	put_unaligned_le32(0xa1b2c3d4, buf + 1);
	KT_EQ(buf[1], 0xd4);
	KT_EQ(get_unaligned_le32(buf + 1), 0xa1b2c3d4);
	KT_EQ(get_unaligned_be16(buf + 1), 0xd4c3);

	/* cadenas */
	KT_EQ(strlen("hola"), 4);
	KT_EQ(strcmp("abc", "abd") < 0, 1);
	KT_EQ(strncmp("abcx", "abcy", 3), 0);
	KT_EQ(strcasecmp("HoLa", "hola"), 0);
	KT_EQ(strscpy(s, "0123456789", 5), -E2BIG);
	KT_STREQ(s, "0123");
	KT_EQ(strscpy(s, "ok"), 2);
	KT_STREQ(strchr("a=b", '=') , "=b");
	KT_STREQ(strrchr("a/b/c", '/'), "/c");
	KT_STREQ(strstr("buscar aguja aqui", "aguja"), "aguja aqui");
	strscpy(s, "  espacios \n");
	KT_STREQ(strim(s), "espacios");
	strscpy(s, "uno,dos,,tres");
	p = s;
	tok = strsep(&p, ",");
	KT_STREQ(tok, "uno");
	tok = strsep(&p, ",");
	KT_STREQ(tok, "dos");
	KT_EQ(sysfs_streq("on\n", "on"), 1);
	{
		static const char *const opts[] = { "off", "auto", "on" };
		KT_EQ(match_string(opts, ARRAY_SIZE(opts), "on"), 2);
		KT_EQ(match_string(opts, ARRAY_SIZE(opts), "x"), -EINVAL);
	}
	KT_EQ(hex2bin(bin, "0aFf", 2), 0);
	KT_EQ(bin[0] + bin[1], 0x0a + 0xff);
	*bin2hex(s, bin, 2) = '\0';
	KT_STREQ(s, "0aff");
	KT_STREQ(str_yes_no(true), "yes");
	KT_EQ(str_has_prefix("drm:card0", "drm:"), 4);
	KT_EQ(isxdigit('F') && !isdigit('a') && tolower('Q') == 'q', 1);
	p = kstrdup("duplicado", GFP_KERNEL);
	KT_STREQ(p, "duplicado");
	kfree(p);
	p = kmemdup("abc", 4, GFP_KERNEL);
	KT_STREQ(p, "abc");
	kfree(p);
	{
		int argc;
		char **argv = argv_split(GFP_KERNEL, " a  bb ccc ", &argc);
		KT_EQ(argc, 3);
		KT_STREQ(argv[2], "ccc");
		argv_free(argv);
	}

	/* sort + bsearch */
	for (int i = 0; i < 64; i++)
		arr[i] = (int)(kt_rand(&seed) % 1000);
	sort(arr, 64, sizeof(int), cmp_int, NULL);
	{
		bool sorted = true;
		for (int i = 1; i < 64; i++)
			if (arr[i - 1] > arr[i])
				sorted = false;
		KT_ASSERT(sorted, "sort no ordena");
	}
	key = arr[37];
	found = bsearch(&key, arr, 64, sizeof(int), cmp_int);
	KT_ASSERT(found && *found == key, "bsearch");

	/* CRC y hash (valores de referencia) */
	KT_EQ(crc32_le(~0U, "123456789", 9) ^ ~0U, 0xCBF43926);
	KT_EQ(crc32c(~0U, "123456789", 9) ^ ~0U, 0xE3069283);
	KT_EQ(crc32_be(~0U, "123456789", 9) ^ ~0U, 0xFC891918);
	KT_EQ(jhash("abc", 3, 0), jhash("abc", 3, 0));
	KT_ASSERT(jhash("abc", 3, 0) != jhash("abd", 3, 0), "jhash");

	/* tiempo */
	{
		ktime_t t0 = ktime_get(), t1;
		unsigned long j0 = jiffies;
		for (volatile int k = 0; k < 2000000; k++)
			;
		t1 = ktime_get();
		KT_ASSERT(ktime_after(t1, t0) || t1 == t0, "ktime no monotono");
		KT_ASSERT(time_after_eq(jiffies, j0), "jiffies");
		KT_EQ(msecs_to_jiffies(250), 250);
		KT_EQ(jiffies_to_usecs(3), 3000);
		KT_EQ(ktime_to_ms(ms_to_ktime(42)), 42);
		KT_ASSERT(ktime_get_real_seconds() > 1600000000, "reloj real: %lld",
			  (long long)ktime_get_real_seconds());
	}
}

static int __init test_k0_init(void)
{
	test_printf();
	test_conv();
	test_bits();
	test_atomic_math();
	test_misc();
	return 0;
}

static void __exit test_k0_exit(void)
{
}

module_init(test_k0_init);
module_exit(test_k0_exit);
MODULE_LICENSE("GPL");
