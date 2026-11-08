// kernel/x86_intrin.h — <immintrin.h> para codigo freestanding.
// xmmintrin.h incluye <mm_malloc.h>, que pide <stdlib.h> (no existe en un cross
// x86_64-elf sin libc). El kernel no usa _mm_malloc/_mm_free: se marca la
// cabecera como ya incluida antes de traer los intrinsecos.
#pragma once
#ifndef _MM_MALLOC_H_INCLUDED
#define _MM_MALLOC_H_INCLUDED
#endif
#include <immintrin.h>
