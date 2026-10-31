/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * struct page de la capa. No es el struct page de Linux: los drivers solo
 * deben usarlo a traves de las funciones de mm.h (page_address,
 * page_to_pfn, get_page...). Las paginas de lkpi_alloc_pages() de orden n
 * vienen en un array contiguo (page + i funciona dentro de la asignacion);
 * pfn_to_page()/virt_to_page() de una pagina ajena crean un descriptor
 * suelto.
 */
#ifndef _LKPI_LINUX_MM_TYPES_H
#define _LKPI_LINUX_MM_TYPES_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/rwsem.h>

struct address_space;
struct kmem_cache;

struct page {
	unsigned long flags;		/* PG_* */
	unsigned long pfn;		/* marco fisico */
	void *virtual;			/* direccion en la ventana directa */
	atomic_t _refcount;
	atomic_t _mapcount;
	unsigned int order;		/* orden de la asignacion (cabeza) */
	unsigned int lkpi_flags;	/* LKPI_PAGE_* (internos) */
	struct page *head;		/* cabeza de la asignacion */
	unsigned long private;
	struct address_space *mapping;
	pgoff_t index;
	struct list_head lru;
	/* slab de la capa (paginas de kmalloc/kmem_cache) */
	struct kmem_cache *slab_cache;
	void *freelist;
	unsigned int inuse;
	struct page *hash_next;		/* registro pfn -> page */
};

struct folio {
	struct page page;
};

/*
 * K8 (L4). pgprot_t lleva bits de PTE de x86 con la PAT de Aurora:
 * indice 0 WB, 1 WT (PWT), 2 UC- (PCD), 3 WC (PWT|PCD).
 */
typedef struct { unsigned long pgprot; } pgprot_t;
#define pgprot_val(x) ((x).pgprot)
#define __pgprot(x) ((pgprot_t){ (x) })
#define _PAGE_PRESENT 0x001UL
#define _PAGE_RW 0x002UL
#define _PAGE_USER 0x004UL
#define _PAGE_PWT 0x008UL
#define _PAGE_PCD 0x010UL
#define _PAGE_NX (1UL << 63)
#define _PAGE_CACHE_MASK (_PAGE_PWT | _PAGE_PCD)

/*
 * Espacio de direcciones de un proceso de Aurora (uno por process_t, lo crea
 * la capa al pedir current->mm). mmap_lock lo toman los drivers; Aurora no
 * lo conoce (sus mmap/munmap usan su propio cerrojo).
 */
enum { MM_FILEPAGES, MM_ANONPAGES, MM_SWAPENTS, MM_SHMEMPAGES, NR_MM_COUNTERS };
struct mm_rss_stat { atomic_long_t count[NR_MM_COUNTERS]; };

struct mm_struct {
	struct mm_rss_stat rss_stat;
	atomic_t mm_users;
	atomic_t mm_count;
	struct rw_semaphore mmap_lock;
	unsigned long task_size;
	void *__lkpi_proc;		/* process_t de Aurora */
	u32 __lkpi_pid;
	struct mm_struct *__lkpi_hnext;
};

typedef unsigned int vm_fault_t;
typedef unsigned long vm_flags_t;

struct file;
struct vm_area_struct;
struct vm_fault {
	struct vm_area_struct *vma;
	unsigned int flags;		/* FAULT_FLAG_* */
	pgoff_t pgoff;			/* pagina dentro del objeto (vm_pgoff incluido) */
	unsigned long address;		/* alineada a pagina */
	unsigned long real_address;
	struct page *page;		/* lo que devuelve ->fault si no mapea el */
	struct page *cow_page;
	gfp_t gfp_mask;
};

struct vm_operations_struct {
	void (*open)(struct vm_area_struct *area);
	void (*close)(struct vm_area_struct *area);
	int (*may_split)(struct vm_area_struct *area, unsigned long addr);
	int (*mremap)(struct vm_area_struct *area);
	vm_fault_t (*fault)(struct vm_fault *vmf);
	vm_fault_t (*huge_fault)(struct vm_fault *vmf, unsigned int order);
	vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
	vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);
	int (*access)(struct vm_area_struct *vma, unsigned long addr, void *buf, int len,
		      int write);
	const char *(*name)(struct vm_area_struct *vma);
};

/*
 * Un vm_area_struct por VMA_DEV de Aurora. vm_start/vm_end/vm_pgoff se
 * refrescan desde el VMA de Aurora antes de cada llamada al driver (un
 * munmap puede recortarlo sin avisar, como en Linux).
 */
struct vm_area_struct {
	unsigned long vm_start;
	unsigned long vm_end;
	struct mm_struct *vm_mm;
	pgprot_t vm_page_prot;
	union {
		const vm_flags_t vm_flags;
		vm_flags_t __vm_flags;
	};
	unsigned long vm_pgoff;
	struct file *vm_file;
	void *vm_private_data;
	const struct vm_operations_struct *vm_ops;
	void *__lkpi_avma;		/* vma_t de Aurora */
	struct list_head __lkpi_mapping; /* en el address_space del fichero */
};

#endif
