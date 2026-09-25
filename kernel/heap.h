#pragma once
#include <stddef.h>
#include <stdint.h>

// Inicializar el heap en la region virtual HEAP_VMA
void heap_init(void);

// Alocar/liberar memoria del kernel (como malloc/free)
void *kmalloc(size_t size);
void kfree(void *ptr);

// --- Wrappers estilo kernel ---

// kmalloc + memset(0). Devuelve NULL si size == 0 o no hay memoria.
void *kzalloc(size_t size);

// kmalloc(nmemb * size) con overflow check + memset(0).
// Devuelve NULL si nmemb * size desborda size_t, o si nmemb o size son 0.
void *kcalloc(size_t nmemb, size_t size);

// Reasigna. Si ptr == NULL, equivale a kmalloc(new_size).
// Si new_size == 0, libera ptr y devuelve NULL.
// Si no, asigna nuevo bloque, copia min(old_size, new_size), libera ptr.
void *krealloc(void *ptr, size_t new_size);

// Duplica una string NUL-terminada. Devuelve NULL si s == NULL o sin memoria.
char *kstrdup(const char *s);

// Duplica hasta n bytes, garantizando NUL-terminador.
// Si s tiene menos de n bytes, copia hasta el NUL.
char *kstrndup(const char *s, size_t n);

uint64_t heap_vma_start(void);
uint64_t heap_vma_end(void);

// Debug: imprimir estado del heap por serial
void heap_dump(void);