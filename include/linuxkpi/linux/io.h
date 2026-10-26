/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MMIO. ioremap* mapea la region en la ventana MMIO de Aurora con el
 * atributo de cache pedido (UC por defecto, WC con ioremap_wc). Los
 * accesos readX/writeX son volatiles; en x86 no necesitan barreras extra.
 * Puertos de E/S (inb/outb...) tambien aqui.
 */
#ifndef _LKPI_LINUX_IO_H
#define _LKPI_LINUX_IO_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/string.h>
#include <asm/page.h>

#define LKPI_IOREMAP_UC 0
#define LKPI_IOREMAP_WC 1
#define LKPI_IOREMAP_WB 2
#define LKPI_IOREMAP_WT 3

void __iomem *lkpi_ioremap(phys_addr_t phys, size_t size, int type);
void lkpi_iounmap(volatile void __iomem *addr);

static inline void __iomem *ioremap(phys_addr_t offset, size_t size)
{
	return lkpi_ioremap(offset, size, LKPI_IOREMAP_UC);
}
#define ioremap_uc ioremap
static inline void __iomem *ioremap_wc(phys_addr_t offset, size_t size)
{
	return lkpi_ioremap(offset, size, LKPI_IOREMAP_WC);
}
static inline void __iomem *ioremap_wt(phys_addr_t offset, size_t size)
{
	return lkpi_ioremap(offset, size, LKPI_IOREMAP_WT);
}
static inline void __iomem *ioremap_cache(phys_addr_t offset, size_t size)
{
	return lkpi_ioremap(offset, size, LKPI_IOREMAP_WB);
}
#define ioremap_encrypted ioremap_cache
#define ioremap_np ioremap
static inline void iounmap(volatile void __iomem *addr) { lkpi_iounmap(addr); }

enum { MEMREMAP_WB = 1 << 0, MEMREMAP_WT = 1 << 1, MEMREMAP_WC = 1 << 2,
       MEMREMAP_ENC = 1 << 3, MEMREMAP_DEC = 1 << 4 };
void *lkpi_memremap(phys_addr_t offset, size_t size, unsigned long flags);
void lkpi_memunmap(void *addr);
#define memremap lkpi_memremap
#define memunmap lkpi_memunmap

#define __LKPI_MMIO(bwlq, type) \
static __always_inline type __raw_read##bwlq(const volatile void __iomem *addr) \
{ return *(const volatile type __force *)addr; } \
static __always_inline void __raw_write##bwlq(type val, volatile void __iomem *addr) \
{ *(volatile type __force *)addr = val; } \
static __always_inline type read##bwlq(const volatile void __iomem *addr) \
{ type v = __raw_read##bwlq(addr); barrier(); return v; } \
static __always_inline void write##bwlq(type val, volatile void __iomem *addr) \
{ barrier(); __raw_write##bwlq(val, addr); } \
static __always_inline type read##bwlq##_relaxed(const volatile void __iomem *addr) \
{ return __raw_read##bwlq(addr); } \
static __always_inline void write##bwlq##_relaxed(type val, volatile void __iomem *addr) \
{ __raw_write##bwlq(val, addr); }
__LKPI_MMIO(b, u8)
__LKPI_MMIO(w, u16)
__LKPI_MMIO(l, u32)
__LKPI_MMIO(q, u64)
#undef __LKPI_MMIO
#define IOMEM_ERR_PTR(err) ((__force void __iomem *)ERR_PTR(err))
#define mmiowb() barrier()

static inline void memcpy_fromio(void *dst, const volatile void __iomem *src, size_t n)
{
	for (size_t i = 0; i < n; i++)
		((u8 *)dst)[i] = readb((const volatile u8 __iomem *)src + i);
}
static inline void memcpy_toio(volatile void __iomem *dst, const void *src, size_t n)
{
	for (size_t i = 0; i < n; i++)
		writeb(((const u8 *)src)[i], (volatile u8 __iomem *)dst + i);
}
static inline void memset_io(volatile void __iomem *dst, int c, size_t n)
{
	for (size_t i = 0; i < n; i++)
		writeb((u8)c, (volatile u8 __iomem *)dst + i);
}
static inline void __iowrite32_copy(void __iomem *to, const void *from, size_t count)
{
	for (size_t i = 0; i < count; i++)
		writel(((const u32 *)from)[i], (volatile u32 __iomem *)to + i);
}
static inline void __ioread32_copy(void *to, const void __iomem *from, size_t count)
{
	for (size_t i = 0; i < count; i++)
		((u32 *)to)[i] = readl((const volatile u32 __iomem *)from + i);
}
static inline void __iowrite64_copy(void __iomem *to, const void *from, size_t count)
{
	for (size_t i = 0; i < count; i++)
		writeq(((const u64 *)from)[i], (volatile u64 __iomem *)to + i);
}

/* Puertos de E/S */
static __always_inline u8 inb(u16 port)
{ u8 v; __asm__ __volatile__("inb %w1, %b0" : "=a"(v) : "Nd"(port)); return v; }
static __always_inline u16 inw(u16 port)
{ u16 v; __asm__ __volatile__("inw %w1, %w0" : "=a"(v) : "Nd"(port)); return v; }
static __always_inline u32 inl(u16 port)
{ u32 v; __asm__ __volatile__("inl %w1, %0" : "=a"(v) : "Nd"(port)); return v; }
static __always_inline void outb(u8 v, u16 port)
{ __asm__ __volatile__("outb %b0, %w1" : : "a"(v), "Nd"(port)); }
static __always_inline void outw(u16 v, u16 port)
{ __asm__ __volatile__("outw %w0, %w1" : : "a"(v), "Nd"(port)); }
static __always_inline void outl(u32 v, u16 port)
{ __asm__ __volatile__("outl %0, %w1" : : "a"(v), "Nd"(port)); }
#define inb_p inb
#define outb_p outb

/*
 * ioread/iowrite (lib/iomap.c en Linux): valen para MMIO y para los puertos
 * que devuelven pci_iomap()/ioport_map() de un BAR de E/S. Un puerto se
 * codifica como LKPI_PIO_OFFSET + puerto, por debajo de LKPI_PIO_RESERVED
 * (nunca es una direccion MMIO valida en la mitad alta).
 */
#define LKPI_PIO_OFFSET 0x10000UL
#define LKPI_PIO_RESERVED 0x40000UL
static __always_inline bool lkpi_iomap_is_port(const volatile void __iomem *addr)
{
	unsigned long a = (unsigned long)addr;

	return a >= LKPI_PIO_OFFSET && a < LKPI_PIO_RESERVED;
}
static __always_inline u16 lkpi_iomap_port(const volatile void __iomem *addr)
{
	return (u16)((unsigned long)addr - LKPI_PIO_OFFSET);
}
static inline void __iomem *ioport_map(unsigned long port, unsigned int nr)
{
	return port > 0xffff ? NULL : (void __iomem *)(port + LKPI_PIO_OFFSET);
}
static inline void ioport_unmap(void __iomem *addr) { }
#define __LKPI_IOMAP(sfx, type, rd, wr, inx, outx) \
static inline type ioread##sfx(const volatile void __iomem *addr) \
{ \
	if (lkpi_iomap_is_port(addr)) \
		return inx(lkpi_iomap_port(addr)); \
	return rd(addr); \
} \
static inline void iowrite##sfx(type val, volatile void __iomem *addr) \
{ \
	if (lkpi_iomap_is_port(addr)) \
		outx(val, lkpi_iomap_port(addr)); \
	else \
		wr(val, addr); \
}
__LKPI_IOMAP(8, u8, readb, writeb, inb, outb)
__LKPI_IOMAP(16, u16, readw, writew, inw, outw)
__LKPI_IOMAP(32, u32, readl, writel, inl, outl)
#undef __LKPI_IOMAP
#define ioread64(a) readq(a)
#define iowrite64(v, a) writeq(v, a)
#define ioread16be(a) be16_to_cpu(ioread16(a))
#define ioread32be(a) be32_to_cpu(ioread32(a))
#define iowrite16be(v, a) iowrite16(cpu_to_be16(v), a)
#define iowrite32be(v, a) iowrite32(cpu_to_be32(v), a)

#define arch_phys_wc_add(base, size) 0
#define arch_phys_wc_del(handle) do { } while (0)
#define arch_io_reserve_memtype_wc(base, size) 0
#define arch_io_free_memtype_wc(base, size) do { } while (0)

#endif
