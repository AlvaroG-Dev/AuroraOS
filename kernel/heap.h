#pragma once
#include <stddef.h>
#include <stdint.h>

// Inicializar el heap en la region virtual HEAP_VMA
void heap_init(void);

// Alocar/liberar memoria del kernel (como malloc/free)
void *kmalloc(size_t size);
void  kfree(void *ptr);

// Debug: imprimir estado del heap por serial
void heap_dump(void);
