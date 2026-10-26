// SPDX-License-Identifier: GPL-2.0-only
/*
 * vsnprintf y familia para la capa LinuxKPI: formatos C99 (d i u o x X c s
 * p n %, banderas - + espacio # 0, anchura y precision con *, longitudes
 * hh h l ll z t j L q) y extensiones %p de Linux:
 *   %px %pK %p    direccion (16 cifras hex; sin hash: Aurora no usa KASLR)
 *   %pe           nombre del error de un ERR_PTR ("-ENOMEM")
 *   %pS %ps %pB   direccion de codigo (sin tabla de simbolos: 0x...)
 *   %pa[p] %pad   phys_addr_t / dma_addr_t por referencia
 *   %pI4 %pi4     IPv4; %pM %pMR %pm  MAC
 *   %*ph[CDN]     volcado hex de hasta 64 bytes
 *   %pV           struct va_format anidado
 *   %p4cc         codigo FourCC (formatos DRM)
 * Lo demas de %p imprime la direccion.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/string.h>

const char *lkpi_errname(int err);

#define F_LEFT 0x01
#define F_PLUS 0x02
#define F_SPACE 0x04
#define F_SPECIAL 0x08
#define F_ZEROPAD 0x10
#define F_SIGN 0x20
#define F_LARGE 0x40	/* hex en mayusculas */

struct out {
	char *buf;
	size_t size;
	size_t pos;		/* bytes que se habrian escrito */
};

static void put(struct out *o, char c)
{
	if (o->pos + 1 < o->size)
		o->buf[o->pos] = c;
	o->pos++;
}

static void put_str(struct out *o, const char *s, int width, int prec, int flags)
{
	int len = 0;
	if (!s)
		s = "(null)";
	while ((prec < 0 || len < prec) && s[len])
		len++;
	if (!(flags & F_LEFT))
		while (len < width--)
			put(o, ' ');
	for (int i = 0; i < len; i++)
		put(o, s[i]);
	while (len < width--)
		put(o, ' ');
}

static void put_num(struct out *o, unsigned long long num, int base, int width,
		    int prec, int flags)
{
	const char *digits = (flags & F_LARGE) ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	char sign = 0;
	int i = 0;

	if (flags & F_SIGN) {
		if ((long long)num < 0) {
			sign = '-';
			num = -(long long)num;
		} else if (flags & F_PLUS) {
			sign = '+';
		} else if (flags & F_SPACE) {
			sign = ' ';
		}
	}
	if (num == 0 && prec != 0)
		tmp[i++] = '0';
	while (num) {
		tmp[i++] = digits[num % base];
		num /= base;
	}
	int ndigits = i > prec ? i : prec;
	int prefix = 0;
	if (flags & F_SPECIAL) {
		if (base == 16)
			prefix = 2;
		else if (base == 8 && (ndigits == 0 || tmp[i - 1] != '0'))
			prefix = 1;
	}
	int total = ndigits + (sign ? 1 : 0) + prefix;
	if (!(flags & (F_LEFT | F_ZEROPAD)))
		while (total < width--)
			put(o, ' ');
	if (sign)
		put(o, sign);
	if (prefix) {
		put(o, '0');
		if (base == 16)
			put(o, (flags & F_LARGE) ? 'X' : 'x');
	}
	if ((flags & F_ZEROPAD) && !(flags & F_LEFT))
		while (total < width--)
			put(o, '0');
	for (int z = i; z < prec; z++)
		put(o, '0');
	while (i > 0)
		put(o, tmp[--i]);
	while (total < width--)
		put(o, ' ');
}

static void put_ptr_raw(struct out *o, const void *p, int width, int flags)
{
	if (width < 0)
		width = 16;
	put_num(o, (unsigned long)p, 16, width, -1, (flags & F_LEFT) | F_ZEROPAD);
}

static int lkpi_vformat(struct out *o, const char *fmt, va_list args);

/* Extensiones %p. Devuelve el puntero al ultimo caracter consumido de fmt. */
static const char *put_pointer(struct out *o, const char *fmt, void *ptr,
			       int width, int prec, int flags)
{
	char c = *fmt;

	switch (c) {
	case 'e': {
		long err = PTR_ERR(ptr);
		const char *name = IS_ERR(ptr) ? lkpi_errname((int)err) : NULL;
		if (name)
			put_str(o, name, width, -1, flags);
		else if (IS_ERR(ptr))
			put_num(o, err, 10, width, -1, flags | F_SIGN);
		else
			put_ptr_raw(o, ptr, -1, 0);
		return fmt;
	}
	case 'a': {
		u64 v = ptr ? *(u64 *)ptr : 0;
		if (fmt[1] == 'd' || fmt[1] == 'p')
			fmt++;
		put_num(o, v, 16, 18, -1, F_SPECIAL | F_ZEROPAD);
		return fmt;
	}
	case 'I':
	case 'i':
		if (fmt[1] == '4') {
			const u8 *a = ptr;
			for (int k = 0; k < 4; k++) {
				if (k)
					put(o, '.');
				put_num(o, a[k], 10, c == 'i' ? 3 : 0, -1, c == 'i' ? F_ZEROPAD : 0);
			}
			return fmt + 1;
		}
		if (fmt[1] == '6')
			fmt++;
		put_ptr_raw(o, ptr, -1, 0);
		return fmt;
	case 'M':
	case 'm': {
		const u8 *a = ptr;
		bool rev = fmt[1] == 'R';
		if (fmt[1] == 'R' || fmt[1] == 'F')
			fmt++;
		for (int k = 0; k < 6; k++) {
			if (k && c == 'M')
				put(o, ':');
			put_num(o, a[rev ? 5 - k : k], 16, 2, -1, F_ZEROPAD);
		}
		return fmt;
	}
	case 'h': {
		char sep = ' ';
		const u8 *a = ptr;
		if (fmt[1] == 'C') { sep = ':'; fmt++; }
		else if (fmt[1] == 'D') { sep = '-'; fmt++; }
		else if (fmt[1] == 'N') { sep = 0; fmt++; }
		int n = width < 0 ? 1 : (width > 64 ? 64 : width);
		for (int k = 0; k < n; k++) {
			if (k && sep)
				put(o, sep);
			put_num(o, a[k], 16, 2, -1, F_ZEROPAD);
		}
		return fmt;
	}
	case 'V': {
		struct va_format *vaf = ptr;
		va_list va;
		va_copy(va, *vaf->va);
		lkpi_vformat(o, vaf->fmt, va);
		va_end(va);
		return fmt;
	}
	case '4':
		if (fmt[1] == 'c' && fmt[2] == 'c') {
			u32 v = *(const u32 *)ptr;
			u32 val = v & ~(1U << 31);
			for (int k = 0; k < 4; k++) {
				unsigned char ch = val >> (k * 8);
				put(o, isprint(ch) ? ch : '.');
			}
			put_str(o, (v & (1U << 31)) ? " big-endian (0x" : " little-endian (0x",
				-1, -1, 0);
			put_num(o, v, 16, 8, -1, F_ZEROPAD);
			put(o, ')');
			return fmt + 2;
		}
		break;
	case 'S':
	case 's':
	case 'B':
	case 'F':
	case 'f':
		if (fmt[1] == 'R' || fmt[1] == 'b')
			fmt++;
		put_num(o, (unsigned long)ptr, 16, 0, -1, F_SPECIAL);
		return fmt;
	case 'x':
	case 'K':
		put_ptr_raw(o, ptr, width, flags);
		return fmt;
	default:
		break;
	}
	/* %p simple (o extension desconocida: se consumen sus letras). */
	put_ptr_raw(o, ptr, width, flags);
	if (isalnum(c)) {
		while (isalnum(fmt[1]))
			fmt++;
		return fmt;
	}
	return fmt - 1;
}

static int lkpi_vformat(struct out *o, const char *fmt, va_list args)
{
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put(o, *fmt);
			continue;
		}
		const char *start = fmt;
		int flags = 0, width = -1, prec = -1, qual = 0;

		for (;;) {
			fmt++;
			if (*fmt == '-') flags |= F_LEFT;
			else if (*fmt == '+') flags |= F_PLUS;
			else if (*fmt == ' ') flags |= F_SPACE;
			else if (*fmt == '#') flags |= F_SPECIAL;
			else if (*fmt == '0') flags |= F_ZEROPAD;
			else break;
		}
		if (isdigit(*fmt)) {
			width = 0;
			while (isdigit(*fmt))
				width = width * 10 + (*fmt++ - '0');
		} else if (*fmt == '*') {
			width = va_arg(args, int);
			if (width < 0) {
				width = -width;
				flags |= F_LEFT;
			}
			fmt++;
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (isdigit(*fmt)) {
				while (isdigit(*fmt))
					prec = prec * 10 + (*fmt++ - '0');
			} else if (*fmt == '*') {
				prec = va_arg(args, int);
				if (prec < 0)
					prec = -1;
				fmt++;
			}
		}
		if (*fmt == 'h' || *fmt == 'l' || *fmt == 'z' || *fmt == 't' ||
		    *fmt == 'j' || *fmt == 'L' || *fmt == 'q' || *fmt == 'Z') {
			qual = *fmt++;
			if (qual == 'h' && *fmt == 'h') { qual = 'H'; fmt++; }
			else if (qual == 'l' && *fmt == 'l') { qual = 'L'; fmt++; }
		}

		unsigned long long num;
		int base = 10;
		switch (*fmt) {
		case 'c':
			if (!(flags & F_LEFT))
				while (--width > 0)
					put(o, ' ');
			put(o, (char)va_arg(args, int));
			while (--width > 0)
				put(o, ' ');
			continue;
		case 's':
			put_str(o, va_arg(args, const char *), width, prec, flags);
			continue;
		case 'p':
			fmt = put_pointer(o, fmt + 1, va_arg(args, void *), width, prec, flags);
			continue;
		case '%':
			put(o, '%');
			continue;
		case 'n':
			/* Linux no lo admite (seguridad); se consume y se ignora. */
			(void)va_arg(args, void *);
			continue;
		case 'o':
			base = 8;
			break;
		case 'X':
			flags |= F_LARGE;
			fallthrough;
		case 'x':
			base = 16;
			break;
		case 'd':
		case 'i':
			flags |= F_SIGN;
			break;
		case 'u':
			break;
		default:
			/* Conversion desconocida: se copia tal cual. */
			for (; start <= fmt && *start; start++)
				put(o, *start);
			if (!*fmt)
				fmt--;
			continue;
		}
		if (qual == 'L')
			num = va_arg(args, unsigned long long);
		else if (qual == 'l' || qual == 'z' || qual == 'Z' || qual == 't' ||
			 qual == 'j' || qual == 'q')
			num = (flags & F_SIGN) ? (unsigned long long)va_arg(args, long)
					       : va_arg(args, unsigned long);
		else if (qual == 'H')
			num = (flags & F_SIGN) ? (unsigned long long)(signed char)va_arg(args, int)
					       : (unsigned char)va_arg(args, unsigned int);
		else if (qual == 'h')
			num = (flags & F_SIGN) ? (unsigned long long)(short)va_arg(args, int)
					       : (unsigned short)va_arg(args, unsigned int);
		else
			num = (flags & F_SIGN) ? (unsigned long long)va_arg(args, int)
					       : va_arg(args, unsigned int);
		if (prec >= 0)
			flags &= ~F_ZEROPAD;
		put_num(o, num, base, width, prec, flags);
	}
	return 0;
}

int lkpi_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	struct out o = { buf, size, 0 };
	va_list ap;

	if (WARN_ON_ONCE(size > INT_MAX))
		return 0;
	va_copy(ap, args);
	lkpi_vformat(&o, fmt, ap);
	va_end(ap);
	if (size)
		buf[o.pos < size ? o.pos : size - 1] = '\0';
	return (int)o.pos;
}
EXPORT_SYMBOL(lkpi_vsnprintf);

int lkpi_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = lkpi_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
EXPORT_SYMBOL(lkpi_snprintf);

int lkpi_vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	int n;
	if (!size)
		return 0;
	n = lkpi_vsnprintf(buf, size, fmt, args);
	return (size_t)n < size ? n : (int)size - 1;
}
EXPORT_SYMBOL(lkpi_vscnprintf);

int lkpi_scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = lkpi_vscnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
EXPORT_SYMBOL(lkpi_scnprintf);

int lkpi_vsprintf(char *buf, const char *fmt, va_list args)
{
	return lkpi_vsnprintf(buf, INT_MAX, fmt, args);
}
EXPORT_SYMBOL(lkpi_vsprintf);

int lkpi_sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = lkpi_vsnprintf(buf, INT_MAX, fmt, ap);
	va_end(ap);
	return n;
}
EXPORT_SYMBOL(lkpi_sprintf);

char *lkpi_kvasprintf(gfp_t gfp, const char *fmt, va_list args)
{
	va_list ap;
	int len;
	char *p;

	va_copy(ap, args);
	len = lkpi_vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	p = kmalloc(len + 1, gfp);
	if (!p)
		return NULL;
	lkpi_vsnprintf(p, len + 1, fmt, args);
	return p;
}
EXPORT_SYMBOL(lkpi_kvasprintf);

const char *lkpi_kvasprintf_const(gfp_t gfp, const char *fmt, va_list args)
{
	return lkpi_kvasprintf(gfp, fmt, args);
}
EXPORT_SYMBOL(lkpi_kvasprintf_const);

char *lkpi_kasprintf(gfp_t gfp, const char *fmt, ...)
{
	va_list ap;
	char *p;
	va_start(ap, fmt);
	p = lkpi_kvasprintf(gfp, fmt, ap);
	va_end(ap);
	return p;
}
EXPORT_SYMBOL(lkpi_kasprintf);

int lkpi_num_to_str(char *buf, int size, unsigned long long num, unsigned int width)
{
	char tmp[24];
	int len = lkpi_snprintf(tmp, sizeof(tmp), "%*llu", width, num);
	if (len > size)
		return 0;
	memcpy(buf, tmp, len);
	return len;
}
EXPORT_SYMBOL(lkpi_num_to_str);

/* ------------------------------------------------------------- sscanf */

/*
 * Subconjunto de vsscanf: %d %i %u %x %X %o %s %c %n %%, anchura, '*'
 * (descartar) y longitudes hh h l ll z.
 */
int lkpi_vsscanf(const char *buf, const char *fmt, va_list args)
{
	const char *str = buf;
	int num = 0;

	while (*fmt) {
		if (isspace(*fmt)) {
			while (isspace(*fmt))
				fmt++;
			while (isspace(*str))
				str++;
			continue;
		}
		if (*fmt != '%' || fmt[1] == '%') {
			if (*fmt == '%')
				fmt++;
			if (*fmt++ != *str++)
				break;
			continue;
		}
		fmt++;
		bool skip = false;
		int width = -1, qual = 0;
		if (*fmt == '*') {
			skip = true;
			fmt++;
		}
		if (isdigit(*fmt)) {
			width = 0;
			while (isdigit(*fmt))
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == 'h' || *fmt == 'l' || *fmt == 'z' || *fmt == 'L') {
			qual = *fmt++;
			if (qual == 'h' && *fmt == 'h') { qual = 'H'; fmt++; }
			else if (qual == 'l' && *fmt == 'l') { qual = 'L'; fmt++; }
		}
		char conv = *fmt++;
		if (!conv)
			break;
		if (conv == 'n') {
			if (!skip)
				*va_arg(args, int *) = str - buf;
			continue;
		}
		if (conv == 'c') {
			int n = width < 0 ? 1 : width;
			char *d = skip ? NULL : va_arg(args, char *);
			while (n-- > 0 && *str)
				if (d)
					*d++ = *str++;
				else
					str++;
			if (!skip)
				num++;
			continue;
		}
		while (isspace(*str))
			str++;
		if (!*str)
			break;
		if (conv == 's') {
			char *d = skip ? NULL : va_arg(args, char *);
			int n = width < 0 ? INT_MAX : width;
			while (*str && !isspace(*str) && n-- > 0) {
				if (d)
					*d++ = *str;
				str++;
			}
			if (d) {
				*d = '\0';
				num++;
			}
			continue;
		}
		int base = 10;
		bool is_sign = false;
		switch (conv) {
		case 'd': is_sign = true; break;
		case 'i': is_sign = true; base = 0; break;
		case 'u': break;
		case 'x': case 'X': base = 16; break;
		case 'o': base = 8; break;
		default: return num;
		}
		char nbuf[32];
		int n = 0, lim = width < 0 ? (int)sizeof(nbuf) - 1 : min(width, (int)sizeof(nbuf) - 1);
		while (n < lim && *str && (isxdigit(*str) || *str == 'x' || *str == 'X' ||
					   (n == 0 && (*str == '-' || *str == '+'))))
			nbuf[n++] = *str++;
		nbuf[n] = '\0';
		char *end;
		unsigned long long v = is_sign ? (unsigned long long)lkpi_simple_strtoll(nbuf, &end, base)
					       : lkpi_simple_strtoull(nbuf, &end, base);
		if (end == nbuf)
			break;
		str -= n - (end - nbuf);
		if (skip)
			continue;
		switch (qual) {
		case 'H': *va_arg(args, char *) = (char)v; break;
		case 'h': *va_arg(args, short *) = (short)v; break;
		case 'l': case 'z': *va_arg(args, long *) = (long)v; break;
		case 'L': *va_arg(args, long long *) = (long long)v; break;
		default: *va_arg(args, int *) = (int)v; break;
		}
		num++;
	}
	return num;
}
EXPORT_SYMBOL(lkpi_vsscanf);

int lkpi_sscanf(const char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = lkpi_vsscanf(buf, fmt, ap);
	va_end(ap);
	return n;
}
EXPORT_SYMBOL(lkpi_sscanf);
