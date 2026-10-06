// kernel/memlayout.h
#pragma once

// Este header también lo procesa cpp para el linker script.
#ifdef __LINKER__
#define VA(x) x
#else
#define VA(x) x##ULL
#endif

#define KERNEL_VMA VA(0xFFFFFFFF80000000)
#define KERNEL_IMAGE_BASE VA(0xFFFFFFFF81000000) // KERNEL_VMA + 16 MB

#define HEAP_VMA VA(0xFFFFFFFF90000000)
#define HEAP_REGION_SIZE VA(0x10000000) // 256 MB
#define HEAP_END (HEAP_VMA + HEAP_REGION_SIZE)

#define SLAB_VMA VA(0xFFFFFFFFA0000000)
#define SLAB_REGION_SIZE VA(0x10000000) // 256 MB
#define SLAB_END (SLAB_VMA + SLAB_REGION_SIZE)

#ifndef __LINKER__
_Static_assert(KERNEL_IMAGE_BASE < HEAP_VMA, "kernel por encima del heap");
_Static_assert(HEAP_END <= SLAB_VMA, "heap solapa con slab");
_Static_assert(SLAB_END > SLAB_VMA, "slab desborda");
#endif