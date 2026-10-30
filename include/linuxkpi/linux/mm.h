/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Paginas y direcciones (K1 sin DMA). La ventana directa de Aurora mapea
 * toda la RAM en PHYS_MAP_BASE; __va/__pa traducen sobre ella.
 * Mapeos de usuario (vm_area_struct, remap_pfn_range...): al final, K8 (L4).
 */
#ifndef _LKPI_LINUX_MM_H
#define _LKPI_LINUX_MM_H

#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/gfp.h>
#include <linux/mm_types.h>
#include <linux/list.h>
#include <linux/sizes.h>
#include <asm/page.h>

#define PG_locked 0
#define PG_reserved 1
#define PG_dirty 2
#define PG_head 3
#define PG_private 4

struct page *lkpi_pfn_to_page(unsigned long pfn);
struct page *lkpi_virt_to_page(const void *addr);
void lkpi_page_release(struct page *page);
phys_addr_t lkpi_virt_to_phys(const void *addr);

static inline unsigned long page_to_pfn(const struct page *page) { return page->pfn; }
static inline struct page *pfn_to_page(unsigned long pfn) { return lkpi_pfn_to_page(pfn); }
static inline void *page_address(const struct page *page) { return page->virtual; }
static inline phys_addr_t page_to_phys(const struct page *page)
{
	return (phys_addr_t)page->pfn << PAGE_SHIFT;
}
#define phys_to_page(phys) pfn_to_page((phys) >> PAGE_SHIFT)
static inline struct page *virt_to_page(const void *addr) { return lkpi_virt_to_page(addr); }
#define virt_to_pfn(kaddr) (__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_virt(pfn) __va((phys_addr_t)(pfn) << PAGE_SHIFT)
#define virt_addr_valid(kaddr) lkpi_virt_addr_valid((const void *)(kaddr))
bool lkpi_virt_addr_valid(const void *addr);
/* Las struct page de la capa no son un array contiguo por pfn (cada pagina de shmem se asigna sola):
 * i915 junta paginas fisicamente contiguas en un mismo sg y page + n apuntaria a basura. */
#define nth_page(page, n) ((n) ? lkpi_pfn_to_page((page)->pfn + (n)) : (page))
#define page_to_virt(page) page_address(page)
#define lowmem_page_address(page) page_address(page)
#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)

static inline int page_count(const struct page *page) { return atomic_read(&page->_refcount); }
static inline int page_ref_count(const struct page *page) { return atomic_read(&page->_refcount); }
static inline void set_page_count(struct page *page, int v) { atomic_set(&page->_refcount, v); }
static inline void init_page_count(struct page *page) { atomic_set(&page->_refcount, 1); }
static inline void get_page(struct page *page) { atomic_inc(&page->_refcount); }
static inline bool get_page_unless_zero(struct page *page)
{
	return atomic_inc_not_zero(&page->_refcount);
}
static inline void put_page(struct page *page)
{
	if (atomic_dec_and_test(&page->_refcount))
		lkpi_page_release(page);
}
static inline void page_ref_inc(struct page *page) { atomic_inc(&page->_refcount); }
static inline void page_ref_dec(struct page *page) { atomic_dec(&page->_refcount); }
static inline int page_mapcount(struct page *page) { return atomic_read(&page->_mapcount) + 1; }
static inline struct page *compound_head(const struct page *page)
{
	return page->head ? page->head : (struct page *)page;
}
static inline unsigned int compound_order(const struct page *page)
{
	return compound_head(page)->order;
}
static inline bool PageReserved(const struct page *page) { return test_bit(PG_reserved, &page->flags); }
static inline void SetPageReserved(struct page *page) { set_bit(PG_reserved, &page->flags); }
static inline void ClearPageReserved(struct page *page) { clear_bit(PG_reserved, &page->flags); }
static inline bool PageDirty(const struct page *page) { return test_bit(PG_dirty, &page->flags); }
static inline void SetPageDirty(struct page *page) { set_bit(PG_dirty, &page->flags); }
static inline void ClearPageDirty(struct page *page) { clear_bit(PG_dirty, &page->flags); }
static inline bool set_page_dirty(struct page *page) { return !test_and_set_bit(PG_dirty, &page->flags); }
#define set_page_dirty_lock set_page_dirty
static inline bool PageHighMem(const struct page *page) { return false; }
static inline bool PageCompound(const struct page *page) { return page->order > 0 || page->head; }
static inline bool PageHead(const struct page *page) { return page->order > 0 && !page->head; }
static inline bool PageTail(const struct page *page) { return page->head != NULL; }
static inline void set_page_private(struct page *page, unsigned long data) { page->private = data; }
static inline unsigned long page_private(const struct page *page) { return page->private; }
static inline struct folio *page_folio(struct page *page) { return (struct folio *)compound_head(page); }
static inline struct page *folio_page(struct folio *folio, unsigned long n) { return &folio->page + n; }
static inline void *folio_address(const struct folio *folio) { return page_address(&folio->page); }
static inline void folio_get(struct folio *folio) { get_page(&folio->page); }
static inline void folio_put(struct folio *folio) { put_page(&folio->page); }
static inline unsigned long folio_nr_pages(const struct folio *folio) { return 1UL << folio->page.order; }
static inline size_t folio_size(const struct folio *folio) { return PAGE_SIZE << folio->page.order; }
static inline unsigned long folio_pfn(const struct folio *folio) { return folio->page.pfn; }
static inline bool want_init_on_free(void) { return false; }
static inline bool want_init_on_alloc(gfp_t flags) { return false; }
/* Parte una asignacion de orden > 0 en paginas sueltas (ttm_pool). */
void lkpi_split_page(struct page *page, unsigned int order);
#define split_page(page, order) lkpi_split_page(page, order)
#define page_size(page) (PAGE_SIZE << compound_order(page))
#define totalram_pages() lkpi_totalram_pages()
unsigned long lkpi_totalram_pages(void);
struct sysinfo {
	unsigned long totalram, freeram, sharedram, bufferram;
	unsigned long totalswap, freeswap;
	unsigned long totalhigh, freehigh;
	unsigned int mem_unit;
};
void lkpi_si_meminfo(struct sysinfo *si);
#define si_meminfo(i) lkpi_si_meminfo(i)
#define is_vmalloc_addr(x) lkpi_is_vmalloc_addr((const void *)(x))
bool lkpi_is_vmalloc_addr(const void *x);

static inline int get_order(unsigned long size)
{
	if (size <= PAGE_SIZE)
		return 0;
	return fls_long((size - 1) >> PAGE_SHIFT);
}

#define PAGE_ALIGN(addr) ALIGN(addr, PAGE_SIZE)
#define PAGE_ALIGN_DOWN(addr) ALIGN_DOWN(addr, PAGE_SIZE)
#define PAGE_ALIGNED(addr) IS_ALIGNED((unsigned long)(addr), PAGE_SIZE)

/*
 * Mapeos de usuario de los drivers (K8, L4): un vm_area_struct por cada
 * VMA_DEV de Aurora (kernel/linuxkpi/mm/mmap.c). Las paginas que se mapean
 * son del driver: Aurora no las libera ni las copia en fork.
 */
#define VM_NONE 0x00000000UL
#define VM_READ 0x00000001UL
#define VM_WRITE 0x00000002UL
#define VM_EXEC 0x00000004UL
#define VM_SHARED 0x00000008UL
#define VM_MAYREAD 0x00000010UL
#define VM_MAYWRITE 0x00000020UL
#define VM_MAYEXEC 0x00000040UL
#define VM_MAYSHARE 0x00000080UL
#define VM_GROWSDOWN 0x00000100UL
#define VM_PFNMAP 0x00000400UL
#define VM_LOCKED 0x00002000UL
#define VM_IO 0x00004000UL
#define VM_SEQ_READ 0x00008000UL
#define VM_RAND_READ 0x00010000UL
#define VM_DONTCOPY 0x00020000UL
#define VM_DONTEXPAND 0x00040000UL
#define VM_LOCKONFAULT 0x00080000UL
#define VM_ACCOUNT 0x00100000UL
#define VM_NORESERVE 0x00200000UL
#define VM_HUGETLB 0x00400000UL
#define VM_SYNC 0x00800000UL
#define VM_WIPEONFORK 0x02000000UL
#define VM_DONTDUMP 0x04000000UL
#define VM_SOFTDIRTY 0
#define VM_MIXEDMAP 0x10000000UL
#define VM_HUGEPAGE 0x20000000UL
#define VM_NOHUGEPAGE 0x40000000UL
#define VM_MERGEABLE 0x80000000UL

#define VM_FAULT_OOM 0x000001
#define VM_FAULT_SIGBUS 0x000002
#define VM_FAULT_MAJOR 0x000004
#define VM_FAULT_HWPOISON 0x000010
#define VM_FAULT_SIGSEGV 0x000040
#define VM_FAULT_NOPAGE 0x000100
#define VM_FAULT_LOCKED 0x000200
#define VM_FAULT_RETRY 0x000400
#define VM_FAULT_FALLBACK 0x000800
#define VM_FAULT_DONE_COW 0x001000
#define VM_FAULT_ERROR (VM_FAULT_OOM | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV | VM_FAULT_HWPOISON)

static inline bool fault_flag_allow_retry_first(unsigned int flags)
{
	return (flags & 0x04) && !(flags & 0x1000);	/* ALLOW_RETRY y no TRIED */
}
#define FAULT_FLAG_WRITE 0x01
#define FAULT_FLAG_MKWRITE 0x02
#define FAULT_FLAG_ALLOW_RETRY 0x04
#define FAULT_FLAG_RETRY_NOWAIT 0x08
#define FAULT_FLAG_KILLABLE 0x10
#define FAULT_FLAG_TRIED 0x1000
#define FAULT_FLAG_USER 0x40
#define FAULT_FLAG_REMOTE 0x80
#define FAULT_FLAG_INTERRUPTIBLE 0x200

#define PAGE_NONE __pgprot(0)
#define PAGE_SHARED __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_NX)
#define PAGE_READONLY __pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_NX)
#define PAGE_KERNEL __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_NX)
#define PAGE_KERNEL_RO __pgprot(_PAGE_PRESENT | _PAGE_NX)
#define PAGE_KERNEL_IO PAGE_KERNEL
#define PAGE_KERNEL_NOCACHE __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_NX | _PAGE_PCD)
static inline pgprot_t pgprot_writecombine(pgprot_t p)
{
	return __pgprot((pgprot_val(p) & ~_PAGE_CACHE_MASK) | _PAGE_PWT | _PAGE_PCD);
}
static inline pgprot_t pgprot_noncached(pgprot_t p)
{
	return __pgprot((pgprot_val(p) & ~_PAGE_CACHE_MASK) | _PAGE_PCD);
}
#define pgprot_device(p) pgprot_noncached(p)
#define pgprot_decrypted(p) (p)
#define pgprot_encrypted(p) (p)
static inline pgprot_t vm_get_page_prot(vm_flags_t flags)
{
	unsigned long v = _PAGE_PRESENT | _PAGE_USER;
	if (flags & VM_WRITE)
		v |= _PAGE_RW;
	if (!(flags & VM_EXEC))
		v |= _PAGE_NX;
	return __pgprot(v);
}

static inline unsigned long vma_pages(const struct vm_area_struct *vma)
{
	return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
}
static inline void vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags)
{
	vma->__vm_flags |= flags;
}
static inline void vm_flags_clear(struct vm_area_struct *vma, vm_flags_t flags)
{
	vma->__vm_flags &= ~flags;
}
static inline void vm_flags_mod(struct vm_area_struct *vma, vm_flags_t set, vm_flags_t clear)
{
	vma->__vm_flags = (vma->__vm_flags | set) & ~clear;
}
static inline void vm_flags_init(struct vm_area_struct *vma, vm_flags_t flags)
{
	vma->__vm_flags = flags;
}
#define vm_flags_reset vm_flags_init
static inline bool is_cow_mapping(vm_flags_t flags)
{
	return (flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE;
}
static inline bool vma_is_anonymous(struct vm_area_struct *vma) { return !vma->vm_ops; }

int lkpi_remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn,
			 unsigned long size, pgprot_t prot);
int lkpi_vm_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page);
vm_fault_t lkpi_vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
				    unsigned long pfn, pgprot_t prot);
void lkpi_zap_vma_ptes(struct vm_area_struct *vma, unsigned long address, unsigned long size);
int lkpi_vm_iomap_memory(struct vm_area_struct *vma, phys_addr_t start, unsigned long len);
#define remap_pfn_range(v, a, p, s, pr) lkpi_remap_pfn_range(v, a, p, s, pr)
#define io_remap_pfn_range(v, a, p, s, pr) lkpi_remap_pfn_range(v, a, p, s, pr)
#define vm_insert_page(v, a, p) lkpi_vm_insert_page(v, a, p)
#define vmf_insert_pfn_prot(v, a, p, pr) lkpi_vmf_insert_pfn_prot(v, a, p, pr)
#define vmf_insert_pfn(v, a, p) lkpi_vmf_insert_pfn_prot(v, a, p, (v)->vm_page_prot)
#define vmf_insert_mixed(v, a, p) lkpi_vmf_insert_pfn_prot(v, a, p, (v)->vm_page_prot)
#define zap_vma_ptes(v, a, s) lkpi_zap_vma_ptes(v, a, s)
#define vm_iomap_memory(v, s, l) lkpi_vm_iomap_memory(v, s, l)
static inline vm_fault_t vmf_insert_page(struct vm_area_struct *vma, unsigned long addr,
					 struct page *page)
{
	int rc = lkpi_vm_insert_page(vma, addr, page);
	if (rc == -ENOMEM)
		return VM_FAULT_OOM;
	if (rc < 0 && rc != -EBUSY)
		return VM_FAULT_SIGBUS;
	return VM_FAULT_NOPAGE;
}
static inline vm_fault_t vmf_error(int err)
{
	return err == -ENOMEM ? VM_FAULT_OOM : VM_FAULT_SIGBUS;
}

/* En Linux lo declara linux/mm.h (drm_vma_manager.h lo usa sin incluir fs.h);
 * misma declaracion y macro que linux/fs.h. */
struct address_space;
void lkpi_unmap_mapping_range(struct address_space *mapping, loff_t holebegin,
			      loff_t holelen, int even_cows);
#define unmap_mapping_range(m, b, l, e) lkpi_unmap_mapping_range(m, b, l, e)

#include <linux/vmalloc.h>
#include <linux/shrinker.h>
#include <linux/mmap_lock.h>


static inline void vma_set_file(struct vm_area_struct *vma, struct file *file)
{
	vma->vm_file = file;
}
static inline struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr) { return NULL; }

/* Sin arbol de VMA de usuario: el iterador no visita nada (userptr/probe_range fallan con -EFAULT). */
struct vma_iterator { struct mm_struct *mm; unsigned long addr; };
#define VMA_ITERATOR(name, _mm, _addr) struct vma_iterator name = { .mm = (_mm), .addr = (_addr) }
#define for_each_vma_range(vmi, vma, end) for ((vma) = NULL; (vma); )
#endif
